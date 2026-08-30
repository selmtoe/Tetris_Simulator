//! Dedicated stack-then-REN controller used only by `KasaneStackRen`.
//!
//! Strategy v2 remains the policy floor. This overlay is entered only when a
//! separate risk/return model selects a persistent two-to-four-column well.
//! While building, the well coordinates are immutable and candidate locks may
//! not add blocks inside it. Once the visible queue proves a sufficiently long
//! REN, control switches to the shared queue-proven REN planner.

use super::guard::project_action_safety;
use super::strategy::{
    best_ren_projection, best_ren_projection_with_limits, choose_queue_proven_ren,
};
use super::{
    forecast_opponent, matured_lines, AgentConfig, Intent, Observation, PolicyOverride,
    SelectedAction, StrategyEvent,
};
use crate::base::{base_features, BaseModel};
use crate::model::{BoardGeometry, DenseLayer};
#[cfg(test)]
use crate::search::legal_actions;
use crate::search::{attack_with_pc, legal_actions_with_hold, PlacementAction};
use anyhow::{bail, Context, Result};
use libtetris::Board;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::fs;
use std::path::Path;

pub const STACK_REN_FEATURE_NAMES: [&str; 56] = [
    "phase_build",
    "phase_fire",
    "well_start",
    "well_width",
    "well_is_edge",
    "own_height",
    "own_headroom_after_due",
    "own_holes",
    "own_covered",
    "own_bumpiness",
    "own_blocks",
    "incoming_now",
    "incoming_due_500",
    "incoming_due_1000",
    "forecast_attack_500",
    "forecast_attack_1000",
    "forecast_attack_2000",
    "forecast_outgoing_500",
    "forecast_outgoing_1000",
    "forecast_outgoing_2000",
    "forecast_max_burst",
    "opponent_attack_eta",
    "opponent_height",
    "opponent_holes",
    "opponent_covered",
    "opponent_blocks",
    "opponent_incoming",
    "opponent_headroom",
    "build_pieces",
    "build_seconds",
    "well_floor",
    "well_inside_max",
    "well_outside_min",
    "well_outside_mean",
    "well_outside_max",
    "well_depth",
    "well_open_volume",
    "well_contamination",
    "well_inside_blocks",
    "well_outside_blocks",
    "well_side_balance",
    "garbage_hole_present",
    "garbage_hole_inside",
    "garbage_hole_at_lowest",
    "garbage_hole_mismatch",
    "projected_ren_chain",
    "projected_ren_attack",
    "projected_ren_efficiency",
    "risk_score",
    "return_score",
    "candidate_height_delta_cc",
    "candidate_holes_delta_cc",
    "candidate_covered_delta_cc",
    "candidate_bumpiness_delta_cc",
    "candidate_raw_attack",
    "candidate_is_clear",
];

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StackRenModel {
    pub schema: String,
    pub feature_names: Vec<String>,
    pub input_mean: Vec<f32>,
    pub input_scale: Vec<f32>,
    pub linear_weights: Vec<f32>,
    pub linear_bias: f32,
    #[serde(default)]
    pub hidden_layers: Vec<DenseLayer>,
    #[serde(default)]
    pub neural_output_weights: Vec<f32>,
    #[serde(default)]
    pub neural_output_bias: f32,
}

impl Default for StackRenModel {
    fn default() -> Self {
        Self::bootstrap()
    }
}

impl StackRenModel {
    pub fn bootstrap() -> Self {
        let names = STACK_REN_FEATURE_NAMES
            .iter()
            .map(|name| (*name).to_owned())
            .collect::<Vec<_>>();
        let mut scale = vec![1.0; names.len()];
        let mut weights = vec![0.0; names.len()];
        let mut set = |name: &str, divisor: f32, weight: f32| {
            let index = STACK_REN_FEATURE_NAMES
                .iter()
                .position(|candidate| *candidate == name)
                .expect("stack-ren bootstrap feature exists");
            scale[index] = divisor;
            weights[index] = weight;
        };

        set("phase_build", 1.0, 0.40);
        set("phase_fire", 1.0, 1.10);
        set("well_width", 4.0, 0.24);
        set("well_is_edge", 1.0, 0.12);
        set("own_height", 10.0, -1.25);
        set("own_headroom_after_due", 10.0, 1.80);
        set("own_holes", 4.0, -4.50);
        set("own_covered", 8.0, -2.20);
        set("own_bumpiness", 12.0, -0.55);
        set("own_blocks", 80.0, 0.32);
        set("incoming_now", 4.0, -1.20);
        set("incoming_due_500", 4.0, -2.10);
        set("incoming_due_1000", 4.0, -1.65);
        set("forecast_attack_500", 4.0, -1.40);
        set("forecast_attack_1000", 5.0, -1.10);
        set("forecast_attack_2000", 8.0, -0.70);
        set("forecast_outgoing_500", 4.0, -1.80);
        set("forecast_outgoing_1000", 5.0, -1.35);
        set("forecast_outgoing_2000", 8.0, -0.85);
        set("forecast_max_burst", 6.0, -0.90);
        set("opponent_attack_eta", 2.0, 0.35);
        set("opponent_height", 10.0, 0.40);
        set("opponent_holes", 5.0, 0.28);
        set("opponent_covered", 10.0, 0.18);
        set("opponent_incoming", 6.0, 0.18);
        set("opponent_headroom", 10.0, -0.42);
        set("build_pieces", 16.0, -0.45);
        set("build_seconds", 6.0, -0.55);
        set("well_depth", 8.0, 3.20);
        set("well_open_volume", 32.0, 2.10);
        set("well_contamination", 8.0, -5.50);
        set("well_inside_blocks", 40.0, -0.35);
        set("well_outside_blocks", 80.0, 0.62);
        set("well_side_balance", 8.0, -0.62);
        set("garbage_hole_inside", 1.0, 2.40);
        set("garbage_hole_at_lowest", 1.0, 4.80);
        set("garbage_hole_mismatch", 1.0, -12.0);
        set("projected_ren_chain", 10.0, 4.40);
        set("projected_ren_attack", 20.0, 5.20);
        set("projected_ren_efficiency", 3.0, 2.10);
        set("risk_score", 20.0, -7.50);
        set("return_score", 25.0, 8.20);
        set("candidate_height_delta_cc", 4.0, -1.30);
        set("candidate_holes_delta_cc", 2.0, -5.00);
        set("candidate_covered_delta_cc", 4.0, -2.80);
        set("candidate_bumpiness_delta_cc", 8.0, -0.75);
        set("candidate_raw_attack", 4.0, -0.35);
        set("candidate_is_clear", 1.0, -1.80);

        let first = fixed_layer(names.len(), 64, 0x5354_4143_4b52_454e, 0.18);
        let second = fixed_layer(64, 32, 0x5245_4e2d_4d4f_4445, 0.16);
        Self {
            schema: "kasane-stack-ren-model/v3-fixed-well-mlp".to_owned(),
            feature_names: names,
            input_mean: vec![0.0; STACK_REN_FEATURE_NAMES.len()],
            input_scale: scale,
            linear_weights: weights,
            linear_bias: 0.0,
            hidden_layers: vec![first, second],
            neural_output_weights: vec![0.0; 32],
            neural_output_bias: 0.0,
        }
    }

    pub fn load(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref();
        let bytes = fs::read(path).with_context(|| format!("failed to read {}", path.display()))?;
        let model = serde_json::from_slice::<Self>(&bytes)
            .with_context(|| format!("failed to parse {}", path.display()))?;
        model.validate()?;
        Ok(model)
    }

    pub fn save(&self, path: impl AsRef<Path>) -> Result<()> {
        self.validate()?;
        fs::write(path, serde_json::to_vec_pretty(self)?)?;
        Ok(())
    }

    pub fn validate(&self) -> Result<()> {
        let expected = STACK_REN_FEATURE_NAMES
            .iter()
            .map(|name| (*name).to_owned())
            .collect::<Vec<_>>();
        if self.feature_names != expected {
            bail!("stack-ren model feature schema does not match this build");
        }
        let width = expected.len();
        if self.input_mean.len() != width
            || self.input_scale.len() != width
            || self.linear_weights.len() != width
        {
            bail!("stack-ren model input dimensions are invalid");
        }
        if self
            .input_scale
            .iter()
            .any(|value| !value.is_finite() || *value <= 0.0)
        {
            bail!("stack-ren model scales must be finite and positive");
        }
        let mut input_width = width;
        for layer in &self.hidden_layers {
            if layer.bias.len() != layer.weights.len()
                || layer.weights.iter().any(|row| row.len() != input_width)
            {
                bail!("stack-ren hidden layer dimensions are invalid");
            }
            input_width = layer.bias.len();
        }
        if !self.hidden_layers.is_empty() && self.neural_output_weights.len() != input_width {
            bail!("stack-ren output dimensions are invalid");
        }
        Ok(())
    }

    pub fn score(&self, raw: &[f32]) -> f32 {
        debug_assert_eq!(raw.len(), self.feature_names.len());
        if raw.len() == 56
            && self.hidden_layers.len() == 2
            && self.hidden_layers[0].weights.len() == 64
            && self.hidden_layers[1].weights.len() == 32
            && self.neural_output_weights.len() == 32
        {
            return self.score_fixed_56x64x32(raw);
        }
        self.score_dynamic(raw)
    }

    /// Allocation-free production path. Stack-REN evaluates this head for up
    /// to dozens of placements per lock, so avoiding three temporary vectors
    /// matters much more than increasing the network width again.
    fn score_fixed_56x64x32(&self, raw: &[f32]) -> f32 {
        let mut normalized = [0.0_f32; 56];
        for index in 0..56 {
            normalized[index] = (raw[index] - self.input_mean[index]) / self.input_scale[index];
        }
        let mut score = self.linear_bias + dot(&self.linear_weights, &normalized);
        let mut hidden64 = [0.0_f32; 64];
        for (index, output) in hidden64.iter_mut().enumerate() {
            *output = (dot(&self.hidden_layers[0].weights[index], &normalized)
                + self.hidden_layers[0].bias[index])
                .max(0.0);
        }
        let mut hidden32 = [0.0_f32; 32];
        for (index, output) in hidden32.iter_mut().enumerate() {
            *output = (dot(&self.hidden_layers[1].weights[index], &hidden64)
                + self.hidden_layers[1].bias[index])
                .max(0.0);
        }
        score += self.neural_output_bias + dot(&self.neural_output_weights, &hidden32);
        score
    }

    fn score_dynamic(&self, raw: &[f32]) -> f32 {
        let mut activations = raw
            .iter()
            .zip(&self.input_mean)
            .zip(&self.input_scale)
            .map(|((&value, &mean), &scale)| (value - mean) / scale)
            .collect::<Vec<_>>();
        let mut score = self.linear_bias + dot(&self.linear_weights, &activations);
        for layer in &self.hidden_layers {
            activations = layer
                .weights
                .iter()
                .zip(&layer.bias)
                .map(|(weights, bias)| (dot(weights, &activations) + bias).max(0.0))
                .collect();
        }
        if !self.hidden_layers.is_empty() {
            score += self.neural_output_bias + dot(&self.neural_output_weights, &activations);
        }
        score
    }
}

fn fixed_layer(input: usize, output: usize, seed: u64, amplitude: f32) -> DenseLayer {
    let mut state = seed;
    let mut weights = vec![vec![0.0; input]; output];
    for row in &mut weights {
        for value in row {
            state = splitmix64(state);
            let centered = ((state >> 48) as f32 / u16::MAX as f32) * 2.0 - 1.0;
            *value = centered * amplitude;
        }
    }
    DenseLayer {
        weights,
        bias: vec![0.0; output],
    }
}

fn splitmix64(mut value: u64) -> u64 {
    value = value.wrapping_add(0x9E37_79B9_7F4A_7C15);
    value = (value ^ (value >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    value ^ (value >> 31)
}

fn dot(left: &[f32], right: &[f32]) -> f32 {
    left.iter().zip(right).map(|(a, b)| a * b).sum()
}

#[cfg(not(target_arch = "wasm32"))]
fn stack_trace(message: impl FnOnce() -> String) {
    use std::sync::OnceLock;
    static ENABLED: OnceLock<bool> = OnceLock::new();
    if *ENABLED.get_or_init(|| std::env::var_os("KASANE_STACK_REN_TRACE").is_some()) {
        eprintln!("{}", message());
    }
}

#[cfg(target_arch = "wasm32")]
fn stack_trace(_message: impl FnOnce() -> String) {}

#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
enum StackRenPhase {
    #[default]
    Neutral,
    Building,
    Firing,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct StackRenMemory {
    phase: StackRenPhase,
    well_start: usize,
    well_width: usize,
    started_ms: u64,
    started_piece: u64,
    build_pieces: u32,
    fire_requested_piece: Option<u64>,
    cooldown_until_piece: u64,
}

impl StackRenMemory {
    pub(crate) fn reset(&mut self) {
        *self = Self::default();
    }

    fn abort(&mut self, pieces: u64, cooldown: u64) {
        self.reset();
        self.cooldown_until_piece = pieces.saturating_add(cooldown);
    }

    fn detail(&self) -> u8 {
        encode_well(self.well_start, self.well_width)
    }
}

fn encode_well(start: usize, width: usize) -> u8 {
    if !(2..=4).contains(&width) || start >= 10 || start + width > 10 {
        return 0;
    }
    ((width as u8) << 4) | start as u8
}

#[derive(Copy, Clone, Debug, Default)]
struct WellMetrics {
    floor: u32,
    inside_max: u32,
    outside_min: u32,
    outside_mean: f32,
    outside_max: u32,
    depth: u32,
    open_volume: u32,
    contamination: u32,
    inside_blocks: u32,
    outside_blocks: u32,
    side_balance: f32,
    garbage_hole_present: bool,
    garbage_hole_inside: bool,
    garbage_hole_at_lowest: bool,
}

#[derive(Copy, Clone, Debug, Default)]
struct ForecastSummary {
    attack_500: u32,
    attack_1000: u32,
    attack_2000: u32,
    outgoing_500: u32,
    outgoing_1000: u32,
    outgoing_2000: u32,
    max_burst: u32,
    eta_ms: u64,
}

#[derive(Copy, Clone, Debug)]
struct WellChoice {
    start: usize,
    width: usize,
    score: f32,
}

pub(crate) fn choose_stack_ren(
    observation: &Observation,
    config: &AgentConfig,
    base_model: &BaseModel,
    model: &StackRenModel,
    fallback: &SelectedAction,
    memory: &mut StackRenMemory,
) -> Option<SelectedAction> {
    if !config.stack_ren_enable {
        return None;
    }
    if observation.rules.perfect_clear_special_attack > 0 && fallback.action.lock.perfect_clear {
        if memory.phase != StackRenPhase::Neutral {
            let detail = memory.detail();
            memory.abort(
                observation.own.pieces,
                config.stack_ren_cooldown_pieces as u64,
            );
            let mut selected = fallback.clone();
            selected.strategy_event = StrategyEvent::StackRenAbort;
            selected.strategy_detail = detail;
            return Some(selected);
        }
        return None;
    }

    let own = BoardGeometry::measure(&observation.own.board);
    let opponent = BoardGeometry::measure(&observation.opponent.board);
    let due_1000 = matured_lines(
        &observation.own.incoming,
        observation.now_ms.saturating_add(1_000),
        observation.rules.garbage_grace_ms,
    );
    let headroom_after_due = 20_i32 - own.max_height as i32 - due_1000 as i32;

    if memory.phase == StackRenPhase::Neutral
        && (observation.own.pieces < memory.cooldown_until_piece
            || observation.own.pieces % config.stack_ren_entry_interval.max(1) as u64 != 0
            || headroom_after_due < config.stack_ren_min_headroom
            || own.holes > config.stack_ren_max_holes
            || due_1000 > config.stack_ren_max_due_1000
            // Stack-REN is a finisher, not an opener.  Charging while the
            // opponent is still low gives them enough time to turn the saved
            // field into ordinary pressure and was the dominant source of
            // losses in held-out leagues.  Reuse the strategy policy's attack
            // window so we enter only after the opponent is genuinely under
            // pressure, while the hard-finish branch below still hands very
            // high boards back to the immediate-fire policy.
            || opponent.max_height < config.strategy_attack_enter_height
            || opponent.max_height >= config.strategy_attack_hard_finish_height)
    {
        return None;
    }

    let forecast = forecast_opponent(observation, config);
    let forecast_summary = summarize_forecast(observation, &forecast);

    // Entry and continuation intentionally use different risk rules.  Do not
    // begin a fresh stack while the opponent can already produce more than the
    // configured immediate-fire budget.  Once committed, a larger forecast is
    // allowed when exact headroom says we can receive it: that is the
    // charge -> receive -> immediate-ignite cancellation-dodge line.
    if memory.phase == StackRenPhase::Neutral
        && forecast_summary.outgoing_1000 > config.stack_ren_max_forecast_1000
    {
        memory.cooldown_until_piece = observation
            .own
            .pieces
            .saturating_add(config.stack_ren_entry_interval.max(1) as u64);
        return None;
    }

    if memory.phase == StackRenPhase::Building
        && memory
            .fire_requested_piece
            .is_some_and(|piece| observation.own.pieces > piece)
    {
        memory.phase = StackRenPhase::Firing;
        memory.fire_requested_piece = None;
    }
    // A forecast above the nominal budget is pressure, not an automatic
    // abort. Stack-REN is specifically meant to keep charging while there is
    // room to receive a burst, then ignite after it lands. Abort only when the
    // forecast excess would consume the configured safety headroom.
    let forecast_excess = forecast_summary
        .outgoing_1000
        .saturating_sub(config.stack_ren_max_forecast_1000);
    let hard_unsafe = headroom_after_due < config.stack_ren_min_headroom
        // Clearing the ignition row can expose several pre-existing garbage
        // holes at once.  That is precisely when a live REN should keep
        // digging; treating those exposed holes as a build defect stopped the
        // combo one piece after ignition.  Hole limits protect only the charge
        // geometry.  Firing is governed by the queue-proven continuation and
        // exact post-rise headroom below.
        || (memory.phase != StackRenPhase::Firing && own.holes > config.stack_ren_max_holes)
        || headroom_after_due - (forecast_excess as i32) < config.stack_ren_min_headroom;

    if memory.phase != StackRenPhase::Neutral && hard_unsafe {
        stack_trace(|| {
            format!(
                "stack-ren abort=hard-unsafe now={} piece={} phase={:?} build={} height={} headroom_due={} holes={} due1000={} forecast1000={} excess={}",
                observation.now_ms,
                observation.own.pieces,
                memory.phase,
                memory.build_pieces,
                own.max_height,
                headroom_after_due,
                own.holes,
                due_1000,
                forecast_summary.outgoing_1000,
                forecast_excess,
            )
        });
        let detail = memory.detail();
        memory.abort(
            observation.own.pieces,
            config.stack_ren_cooldown_pieces as u64,
        );
        let mut selected = fallback.clone();
        selected.strategy_event = StrategyEvent::StackRenAbort;
        selected.strategy_detail = detail;
        return Some(selected);
    }

    if memory.phase == StackRenPhase::Firing {
        if let Some(mut selected) = choose_queue_proven_ren(
            observation,
            config,
            fallback,
            1,
            0,
            config.stack_ren_min_headroom,
            // The ignition root must consume the committed well. Once the
            // combo is live, however, the proven route may clear a row with a
            // placement outside the original columns. Requiring every later
            // tetromino itself to touch the well discarded valid continuations
            // that the ignition proof had already counted.
            None,
        ) {
            selected.strategy_event = StrategyEvent::StackRenContinue;
            selected.intent = Intent::ComboContinue;
            selected.strategy_detail = memory.detail();
            return Some(selected);
        }
        stack_trace(|| {
            format!(
                "stack-ren abort=no-continuation now={} piece={} height={} holes={} due1000={} well={}+{}",
                observation.now_ms,
                observation.own.pieces,
                own.max_height,
                own.holes,
                due_1000,
                memory.well_start,
                memory.well_width,
            )
        });
        let detail = memory.detail();
        memory.abort(
            observation.own.pieces,
            config.stack_ren_cooldown_pieces as u64,
        );
        let mut selected = fallback.clone();
        selected.strategy_event = StrategyEvent::StackRenAbort;
        selected.strategy_detail = detail;
        return Some(selected);
    }

    if memory.phase == StackRenPhase::Neutral {
        if hard_unsafe {
            return None;
        }
        let (ren_chain, ren_attack) = best_ren_projection(observation, config, None);
        // Require a visible precursor, not just an optimistic future-depth
        // estimate.  With a three-chain ignition target, at least two clears
        // must already be queue-proven before we spend tempo on the well.
        // This removed the last held-out entry that fired no net attack and
        // merely converted a win into a timeout.
        if ren_chain.saturating_add(1) < config.stack_ren_min_fire_chain {
            memory.cooldown_until_piece = observation
                .own
                .pieces
                .saturating_add(config.stack_ren_entry_interval.max(1) as u64);
            return None;
        }
        let choice = choose_well(
            observation,
            config,
            model,
            own,
            opponent,
            forecast_summary,
            ren_chain,
            ren_attack,
        )?;
        if choice.score < config.stack_ren_entry_threshold {
            memory.cooldown_until_piece = observation
                .own
                .pieces
                .saturating_add(config.stack_ren_entry_interval.max(1) as u64);
            return None;
        }
        stack_trace(|| {
            format!(
                "stack-ren enter now={} piece={} score={:.2} well={}+{} own_h={} own_holes={} opp_h={} opp_holes={} due1000={} forecast1000={} chain={} attack={}",
                observation.now_ms,
                observation.own.pieces,
                choice.score,
                choice.start,
                choice.width,
                own.max_height,
                own.holes,
                opponent.max_height,
                opponent.holes,
                due_1000,
                forecast_summary.outgoing_1000,
                ren_chain,
                ren_attack,
            )
        });
        memory.phase = StackRenPhase::Building;
        memory.well_start = choice.start;
        memory.well_width = choice.width;
        memory.started_ms = observation.now_ms;
        memory.started_piece = observation.own.pieces;
        memory.build_pieces = 0;
        memory.fire_requested_piece = None;
    }

    memory.build_pieces = observation
        .own
        .pieces
        .saturating_sub(memory.started_piece)
        .min(u32::MAX as u64) as u32;

    let well = well_metrics(&observation.own.board, memory.well_start, memory.well_width);
    let (ren_chain, ren_attack) = best_ren_projection(
        observation,
        config,
        Some((memory.well_start, memory.well_width)),
    );
    let elapsed_ms = observation.now_ms.saturating_sub(memory.started_ms);
    let (risk, reward) = risk_return(
        config,
        own,
        opponent,
        forecast_summary,
        due_1000,
        &well,
        memory.well_width,
        ren_chain,
        ren_attack,
        memory.build_pieces,
        observation.own.average_piece_ms,
    );
    let enough_build = memory.build_pieces >= config.stack_ren_min_build_pieces;
    let queue_proves_fire = ren_chain >= config.stack_ren_min_fire_chain
        && ren_attack >= config.stack_ren_min_fire_attack;
    let target_ready = well.depth >= config.stack_ren_target_depth
        || reward >= risk + config.stack_ren_fire_margin
        || memory.build_pieces >= config.stack_ren_max_build_pieces;
    if enough_build && queue_proves_fire && target_ready {
        if let Some(mut selected) = choose_queue_proven_ren(
            observation,
            config,
            fallback,
            config.stack_ren_min_fire_chain,
            config.stack_ren_min_fire_attack,
            config.stack_ren_min_headroom,
            Some((memory.well_start, memory.well_width)),
        ) {
            stack_trace(|| {
                format!(
                    "stack-ren fire now={} piece={} build={} depth={} chain={} attack={} height={} holes={}",
                    observation.now_ms,
                    observation.own.pieces,
                    memory.build_pieces,
                    well.depth,
                    ren_chain,
                    ren_attack,
                    own.max_height,
                    own.holes,
                )
            });
            memory.fire_requested_piece = Some(observation.own.pieces);
            selected.strategy_event = StrategyEvent::StackRenFire;
            selected.intent = Intent::ComboContinue;
            selected.strategy_detail = memory.detail();
            return Some(selected);
        }
    }

    let build_too_long = memory.build_pieces >= config.stack_ren_max_build_pieces
        || elapsed_ms > config.stack_ren_max_build_ms
        || risk > reward + config.stack_ren_abort_margin;
    if build_too_long {
        stack_trace(|| {
            format!(
                "stack-ren abort=build-limit now={} piece={} build={} elapsed={} depth={} chain={} attack={} risk={:.2} reward={:.2}",
                observation.now_ms,
                observation.own.pieces,
                memory.build_pieces,
                elapsed_ms,
                well.depth,
                ren_chain,
                ren_attack,
                risk,
                reward,
            )
        });
        let detail = memory.detail();
        memory.abort(
            observation.own.pieces,
            config.stack_ren_cooldown_pieces as u64,
        );
        let mut selected = fallback.clone();
        selected.strategy_event = StrategyEvent::StackRenAbort;
        selected.strategy_detail = detail;
        return Some(selected);
    }

    let selected = choose_build_action(
        observation,
        config,
        base_model,
        model,
        fallback,
        memory,
        own,
        opponent,
        forecast_summary,
        ren_chain,
        ren_attack,
        risk,
        reward,
    );
    match selected {
        Some(mut selected) => {
            selected.strategy_event = if memory.build_pieces == 0 {
                StrategyEvent::StackRenEnter
            } else {
                StrategyEvent::StackRenBuild
            };
            Some(selected)
        }
        None => {
            stack_trace(|| {
                format!(
                    "stack-ren abort=no-build-action now={} piece={} build={} depth={} height={} holes={}",
                    observation.now_ms,
                    observation.own.pieces,
                    memory.build_pieces,
                    well.depth,
                    own.max_height,
                    own.holes,
                )
            });
            let detail = memory.detail();
            memory.abort(
                observation.own.pieces,
                config.stack_ren_cooldown_pieces as u64,
            );
            let mut selected = fallback.clone();
            selected.strategy_event = StrategyEvent::StackRenAbort;
            selected.strategy_detail = detail;
            Some(selected)
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn choose_well(
    observation: &Observation,
    config: &AgentConfig,
    model: &StackRenModel,
    own: BoardGeometry,
    opponent: BoardGeometry,
    forecast: ForecastSummary,
    ren_chain: u32,
    ren_attack: u32,
) -> Option<WellChoice> {
    let mut best = None;
    for width in config.stack_ren_min_well_width.max(2)..=config.stack_ren_max_well_width.min(4) {
        if width >= 10 {
            continue;
        }
        for start in 0..=10 - width {
            let metrics = well_metrics(&observation.own.board, start, width);
            let (risk, reward) = risk_return(
                config,
                own,
                opponent,
                forecast,
                matured_lines(
                    &observation.own.incoming,
                    observation.now_ms.saturating_add(1_000),
                    observation.rules.garbage_grace_ms,
                ),
                &metrics,
                width,
                ren_chain,
                ren_attack,
                0,
                observation.own.average_piece_ms,
            );
            let features = stack_features(
                observation,
                StackRenPhase::Building,
                start,
                width,
                0,
                0,
                own,
                opponent,
                forecast,
                metrics,
                ren_chain,
                ren_attack,
                risk,
                reward,
                None,
                None,
            );
            let mut score = model.score(&features) + (reward - risk);
            if metrics.garbage_hole_present && !metrics.garbage_hole_at_lowest {
                score -= config.stack_ren_hole_mismatch_penalty;
            }
            // A clean three-wide leaves a seven-column platform and completed
            // far more often than four-wide in held-out play. Keep every 2..=4
            // family available, but make three-wide the clean-field prior.
            // Existing garbage alignment bypasses these priors so a natural
            // two/four-wide channel can still win on its geometry and NN score.
            let centered = start.abs_diff((10 - width) / 2) <= 1;
            let edge = start == 0 || start + width == 10;
            if !metrics.garbage_hole_present {
                score += match (width, edge, centered) {
                    (3, true, _) => 12.0,
                    (3, false, true) => 7.0,
                    (4, true, _) => 4.0,
                    (4, false, true) => -12.0,
                    (2, true, _) => 2.0,
                    (_, true, _) | (_, false, true) => 1.0,
                    _ => 0.0,
                };
            } else if edge || centered {
                score += 1.0;
            }
            let candidate = WellChoice {
                start,
                width,
                score,
            };
            if best
                .as_ref()
                .map(|current: &WellChoice| candidate.score > current.score)
                .unwrap_or(true)
            {
                best = Some(candidate);
            }
        }
    }
    best
}

#[allow(clippy::too_many_arguments)]
fn choose_build_action(
    observation: &Observation,
    config: &AgentConfig,
    base_model: &BaseModel,
    model: &StackRenModel,
    fallback: &SelectedAction,
    memory: &StackRenMemory,
    own: BoardGeometry,
    opponent: BoardGeometry,
    forecast: ForecastSummary,
    ren_chain: u32,
    ren_attack: u32,
    risk: f32,
    reward: f32,
) -> Option<SelectedAction> {
    let before_well = well_metrics(&observation.own.board, memory.well_start, memory.well_width);
    let fallback_geometry = BoardGeometry::measure(&fallback.action.board_after);
    let allowed_holes = config.stack_ren_max_holes.max(own.holes);
    let mut actions = legal_actions_with_hold(&observation.own.board, observation.own.can_hold)
        .into_iter()
        .map(|action| {
            let base_score = base_model.score(&base_features(&observation.own.board, &action));
            (action, base_score)
        })
        .collect::<Vec<_>>();
    actions.sort_by(|left, right| right.1.partial_cmp(&left.1).unwrap_or(Ordering::Equal));
    let mut candidates = Vec::new();
    let mut valid_actions = 0_usize;
    let mut rejected = [0_u32; 8];
    for (action, base_score) in actions {
        let raw = attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
        if action.lock.locked_out || raw > 0 || !action.lock.cleared_lines.is_empty() {
            rejected[0] += 1;
            continue;
        }
        let geometry = BoardGeometry::measure(&action.board_after);
        let metrics = well_metrics(&action.board_after, memory.well_start, memory.well_width);
        let adds_inside = metrics.inside_blocks > before_well.inside_blocks;
        let may_arm = adds_inside
            && memory.build_pieces >= config.stack_ren_min_build_pieces
            && before_well.depth >= config.stack_ren_target_depth;
        if adds_inside && !may_arm {
            rejected[1] += 1;
            continue;
        }
        if metrics.contamination > before_well.contamination && !may_arm {
            rejected[2] += 1;
            continue;
        }
        if geometry.holes > allowed_holes {
            rejected[3] += 1;
            continue;
        }
        if geometry.covered > own.covered.saturating_add(1) {
            rejected[4] += 1;
            continue;
        }
        let shell = SelectedAction {
            action: action.clone(),
            wait_ms: 0,
            intent: Intent::Charge,
            score: 0.0,
            policy_override: if action.key() == fallback.action.key() {
                PolicyOverride::None
            } else {
                PolicyOverride::Placement
            },
            strategy_event: StrategyEvent::StackRenBuild,
            strategy_detail: encode_well(memory.well_start, memory.well_width),
        };
        let safety = project_action_safety(observation, &shell);
        if safety.locked_out
            || safety.headroom_after_rise < config.stack_ren_min_headroom
            || safety.holes > allowed_holes
            || safety.covered > own.covered.saturating_add(1)
        {
            rejected[5] += 1;
            continue;
        }
        valid_actions += 1;
        if valid_actions > config.stack_ren_action_limit.max(1) {
            break;
        }
        let candidate_features = stack_features(
            observation,
            StackRenPhase::Building,
            memory.well_start,
            memory.well_width,
            memory.build_pieces.saturating_add(1),
            observation.now_ms.saturating_sub(memory.started_ms),
            geometry,
            opponent,
            forecast,
            metrics,
            ren_chain,
            ren_attack,
            risk,
            reward,
            Some(fallback_geometry),
            Some(&action),
        );
        let mut score = model.score(&candidate_features) + base_score * 0.06;
        if metrics.garbage_hole_present && !metrics.garbage_hole_at_lowest {
            score -= config.stack_ren_hole_mismatch_penalty;
        }
        candidates.push((SelectedAction { score, ..shell }, may_arm));
    }
    candidates.sort_by(|left, right| right.0.score.total_cmp(&left.0.score));

    // Geometry alone can create a deep-looking well whose bottom shelf cannot
    // be ignited by the visible queue. Probe only the strongest few candidates
    // after their lock and reward an executable short REN. Most probes end
    // after root move generation when no clear exists, keeping this far below
    // the cost of a full proof search over every legal placement.
    for (rank, (candidate, may_arm)) in candidates.iter_mut().enumerate() {
        if rank >= 6 && !*may_arm {
            continue;
        }
        let safety = project_action_safety(observation, candidate);
        if safety.rise > 0 {
            if *may_arm {
                candidate.score = f32::NEG_INFINITY;
            }
            continue;
        }
        let projected = observation_after_build(observation, &candidate.action);
        let (next_chain, next_attack) = best_ren_projection_with_limits(
            &projected,
            Some((memory.well_start, memory.well_width)),
            config.strategy_ren_depth.min(3),
            config.strategy_ren_beam_width.min(8),
        );
        // A primer may enter the committed well only when the visible queue
        // proves that it leads directly into a real multi-clear REN. This is
        // the arming transition missing from a purely empty four-wide well.
        if *may_arm
            && (next_chain < config.stack_ren_min_fire_chain.max(2)
                || next_attack < config.stack_ren_min_fire_attack)
        {
            rejected[6] += 1;
            candidate.score = f32::NEG_INFINITY;
            continue;
        }
        candidate.score += next_chain as f32 * 140.0 + next_attack as f32 * 35.0;
    }
    let selected = candidates
        .into_iter()
        .filter(|(candidate, _)| candidate.score.is_finite())
        .map(|(candidate, _)| candidate)
        .max_by(|left, right| left.score.total_cmp(&right.score));
    if selected.is_none() {
        stack_trace(|| {
            format!(
                "stack-ren build-filter piece={} well={}+{} before-depth={} rejected(clear,inside,contam,holes,covered,safety,primer,other)={:?}",
                observation.own.pieces,
                memory.well_start,
                memory.well_width,
                before_well.depth,
                rejected,
            )
        });
    }
    selected
}

fn observation_after_build(observation: &Observation, action: &PlacementAction) -> Observation {
    let elapsed_ms = observation.rules.decision_latency_ms.saturating_add(
        observation
            .rules
            .controller_time_ms(action.movements.len(), action.hold),
    );
    let mut projected = observation.clone();
    projected.now_ms = projected.now_ms.saturating_add(elapsed_ms);
    projected.own.board = action.board_after.clone();
    projected.own.can_hold = true;
    projected.own.phase = super::PhaseView::Ready;
    projected.own.pieces = projected.own.pieces.saturating_add(1);
    let piece_ms = elapsed_ms.max(1) as f32;
    projected.own.average_piece_ms = if observation.own.pieces == 0 {
        piece_ms
    } else {
        observation.own.average_piece_ms * 0.9 + piece_ms * 0.1
    };
    projected
}

fn summarize_forecast(
    observation: &Observation,
    forecast: &super::OpponentForecast,
) -> ForecastSummary {
    let sum = |window: u64, outgoing: bool| {
        forecast
            .events
            .iter()
            .filter(|event| event.lock_ms <= observation.now_ms.saturating_add(window))
            .map(|event| {
                if outgoing {
                    event.outgoing_attack
                } else {
                    event.raw_attack
                }
            })
            .sum::<u32>()
    };
    ForecastSummary {
        attack_500: sum(500, false),
        attack_1000: sum(1_000, false),
        attack_2000: sum(2_000, false),
        outgoing_500: sum(500, true),
        outgoing_1000: sum(1_000, true),
        outgoing_2000: sum(2_000, true),
        max_burst: forecast
            .events
            .iter()
            .map(|event| event.outgoing_attack)
            .max()
            .unwrap_or(0),
        eta_ms: forecast
            .events
            .iter()
            .filter(|event| event.outgoing_attack > 0 && event.lock_ms >= observation.now_ms)
            .map(|event| event.lock_ms - observation.now_ms)
            .min()
            .unwrap_or(10_000),
    }
}

#[allow(clippy::too_many_arguments)]
fn risk_return(
    config: &AgentConfig,
    own: BoardGeometry,
    opponent: BoardGeometry,
    forecast: ForecastSummary,
    due_1000: u32,
    well: &WellMetrics,
    well_width: usize,
    ren_chain: u32,
    ren_attack: u32,
    build_pieces: u32,
    average_piece_ms: f32,
) -> (f32, f32) {
    let remaining = config
        .stack_ren_max_build_pieces
        .saturating_sub(build_pieces);
    let expected_added_blocks = remaining.min(12).saturating_mul(4);
    let outside_columns = 10_u32.saturating_sub(well_width.min(9) as u32);
    let future_depth = well
        .depth
        .saturating_add(expected_added_blocks / outside_columns.max(1));
    let estimated_chain = ren_chain.max(future_depth.min(15));
    let estimated_attack = ren_attack.max(ren_attack_estimate(estimated_chain));
    let build_seconds = remaining.min(12) as f32 * average_piece_ms.max(100.0) / 1_000.0;
    let risk = own.max_height as f32 * 0.72
        + own.holes as f32 * 3.8
        + own.covered as f32 * 0.85
        + due_1000 as f32 * 2.1
        + forecast.outgoing_1000 as f32 * 1.7
        + forecast.outgoing_2000 as f32 * 0.55
        + forecast.max_burst as f32 * 0.8
        + build_seconds * 0.75
        + well.contamination as f32 * 2.2;
    let opponent_headroom = 20_i32 - opponent.max_height as i32;
    let lethal = (estimated_attack as i32 - opponent_headroom.max(0)).max(0) as f32;
    let reward = estimated_attack as f32 * 1.15
        + estimated_chain as f32 * 1.55
        + lethal * 3.0
        + opponent.holes as f32 * 0.35
        + opponent.covered as f32 * 0.12
        + well.depth as f32 * 0.8;
    (risk, reward)
}

fn ren_attack_estimate(chain: u32) -> u32 {
    (1..=chain)
        .map(|ren| match ren {
            0..=1 => 0,
            2..=3 => 1,
            4..=5 => 2,
            6..=7 => 3,
            8..=10 => 4,
            _ => 5,
        })
        .sum()
}

fn well_metrics(board: &Board, start: usize, width: usize) -> WellMetrics {
    let heights = board.column_heights();
    let end = (start + width).min(10);
    let inside = &heights[start..end];
    let floor = inside.iter().copied().min().unwrap_or(0).max(0) as u32;
    let inside_max = inside.iter().copied().max().unwrap_or(0).max(0) as u32;
    let mut outside_min = u32::MAX;
    let mut outside_max = 0_u32;
    let mut outside_sum = 0_u32;
    let mut outside_count = 0_u32;
    for (column, height) in heights.iter().enumerate() {
        if column >= start && column < end {
            continue;
        }
        let height = (*height).max(0) as u32;
        outside_min = outside_min.min(height);
        outside_max = outside_max.max(height);
        outside_sum = outside_sum.saturating_add(height);
        outside_count += 1;
    }
    if outside_count == 0 {
        outside_min = 0;
    }
    let outside_mean = outside_sum as f32 / outside_count.max(1) as f32;
    let depth = outside_min.saturating_sub(inside_max);
    let mut inside_blocks = 0;
    let mut outside_blocks = 0;
    for x in 0..10 {
        for y in 0..heights[x].max(0) {
            if board.occupied(x as i32, y) {
                if x >= start && x < end {
                    inside_blocks += 1;
                } else {
                    outside_blocks += 1;
                }
            }
        }
    }
    let contamination = (start..end)
        .map(|x| (heights[x].max(0) as u32).saturating_sub(floor))
        .sum();
    let open_volume = (start..end)
        .map(|x| {
            (floor..outside_min)
                .filter(|y| !board.occupied(x as i32, *y as i32))
                .count() as u32
        })
        .sum();
    let left_mean = side_mean(&heights, 0, start);
    let right_mean = side_mean(&heights, end, 10);
    let side_balance = if start == 0 || end == 10 {
        0.0
    } else {
        (left_mean - right_mean).abs()
    };
    let garbage_hole = deepest_garbage_hole(board);
    let garbage_hole_inside = garbage_hole
        .map(|column| column >= start && column < end)
        .unwrap_or(false);
    let garbage_hole_at_lowest = garbage_hole
        .map(|column| garbage_hole_inside && heights[column].max(0) as u32 == floor)
        .unwrap_or(false);
    WellMetrics {
        floor,
        inside_max,
        outside_min,
        outside_mean,
        outside_max,
        depth,
        open_volume,
        contamination,
        inside_blocks,
        outside_blocks,
        side_balance,
        garbage_hole_present: garbage_hole.is_some(),
        garbage_hole_inside,
        garbage_hole_at_lowest,
    }
}

fn side_mean(heights: &[i32; 10], start: usize, end: usize) -> f32 {
    if start >= end {
        return 0.0;
    }
    heights[start..end]
        .iter()
        .map(|height| (*height).max(0) as f32)
        .sum::<f32>()
        / (end - start) as f32
}

fn deepest_garbage_hole(board: &Board) -> Option<usize> {
    for y in 0..40 {
        let mut hole_count = 0_u8;
        let mut hole_column = 0_usize;
        for x in 0..10 {
            if !board.occupied(x as i32, y) {
                hole_count += 1;
                hole_column = x;
                if hole_count > 1 {
                    break;
                }
            }
        }
        if hole_count == 1 {
            return Some(hole_column);
        }
    }
    None
}

#[allow(clippy::too_many_arguments)]
fn stack_features(
    observation: &Observation,
    phase: StackRenPhase,
    well_start: usize,
    well_width: usize,
    build_pieces: u32,
    build_ms: u64,
    own: BoardGeometry,
    opponent: BoardGeometry,
    forecast: ForecastSummary,
    well: WellMetrics,
    ren_chain: u32,
    ren_attack: u32,
    risk: f32,
    reward: f32,
    fallback: Option<BoardGeometry>,
    action: Option<&PlacementAction>,
) -> [f32; 56] {
    let due_500 = matured_lines(
        &observation.own.incoming,
        observation.now_ms.saturating_add(500),
        observation.rules.garbage_grace_ms,
    );
    let due_1000 = matured_lines(
        &observation.own.incoming,
        observation.now_ms.saturating_add(1_000),
        observation.rules.garbage_grace_ms,
    );
    let headroom = 20_i32 - own.max_height as i32 - due_1000 as i32;
    let opponent_headroom =
        20_i32 - opponent.max_height as i32 - observation.opponent.incoming_total() as i32;
    let fallback = fallback.unwrap_or(own);
    let raw = action
        .map(|candidate| {
            attack_with_pc(
                &candidate.lock,
                observation.rules.perfect_clear_special_attack,
            )
        })
        .unwrap_or(0);
    let is_clear = action
        .map(|candidate| !candidate.lock.cleared_lines.is_empty())
        .unwrap_or(false);
    let mismatch = well.garbage_hole_present && !well.garbage_hole_at_lowest;
    let features = [
        (phase == StackRenPhase::Building) as u8 as f32,
        (phase == StackRenPhase::Firing) as u8 as f32,
        well_start as f32,
        well_width as f32,
        (well_start == 0 || well_start + well_width == 10) as u8 as f32,
        own.max_height as f32,
        headroom as f32,
        own.holes as f32,
        own.covered as f32,
        own.bumpiness as f32,
        own.blocks as f32,
        observation.own.incoming_total() as f32,
        due_500 as f32,
        due_1000 as f32,
        forecast.attack_500 as f32,
        forecast.attack_1000 as f32,
        forecast.attack_2000 as f32,
        forecast.outgoing_500 as f32,
        forecast.outgoing_1000 as f32,
        forecast.outgoing_2000 as f32,
        forecast.max_burst as f32,
        forecast.eta_ms.min(10_000) as f32 / 1_000.0,
        opponent.max_height as f32,
        opponent.holes as f32,
        opponent.covered as f32,
        opponent.blocks as f32,
        observation.opponent.incoming_total() as f32,
        opponent_headroom as f32,
        build_pieces as f32,
        build_ms as f32 / 1_000.0,
        well.floor as f32,
        well.inside_max as f32,
        well.outside_min as f32,
        well.outside_mean,
        well.outside_max as f32,
        well.depth as f32,
        well.open_volume as f32,
        well.contamination as f32,
        well.inside_blocks as f32,
        well.outside_blocks as f32,
        well.side_balance,
        well.garbage_hole_present as u8 as f32,
        well.garbage_hole_inside as u8 as f32,
        well.garbage_hole_at_lowest as u8 as f32,
        mismatch as u8 as f32,
        ren_chain as f32,
        ren_attack as f32,
        ren_attack as f32 / ren_chain.max(1) as f32,
        risk,
        reward,
        own.max_height as f32 - fallback.max_height as f32,
        own.holes as f32 - fallback.holes as f32,
        own.covered as f32 - fallback.covered as f32,
        own.bumpiness as f32 - fallback.bumpiness as f32,
        raw as f32,
        is_clear as u8 as f32,
    ];
    features
}

#[cfg(test)]
mod tests {
    use super::*;
    use libtetris::{Board, Piece};

    fn queued_board() -> Board {
        let mut board = Board::new();
        for piece in [
            Piece::I,
            Piece::O,
            Piece::T,
            Piece::S,
            Piece::Z,
            Piece::L,
            Piece::J,
            Piece::I,
            Piece::O,
        ] {
            board.add_next_piece(piece);
        }
        board
    }

    fn view(board: Board, pieces: u64) -> super::super::PlayerView {
        super::super::PlayerView {
            board,
            can_hold: true,
            incoming: Vec::new(),
            phase: super::super::PhaseView::Ready,
            pieces,
            average_piece_ms: 350.0,
        }
    }

    #[test]
    fn bootstrap_model_schema_is_exact() {
        let model = StackRenModel::bootstrap();
        model.validate().unwrap();
        assert_eq!(model.feature_names.len(), 56);
        assert_eq!(model.hidden_layers[0].bias.len(), 64);
        assert_eq!(model.hidden_layers[1].bias.len(), 32);
    }

    #[test]
    fn allocation_free_score_matches_generic_network() {
        let mut model = StackRenModel::bootstrap();
        for (index, weight) in model.neural_output_weights.iter_mut().enumerate() {
            *weight = index as f32 * 0.013 - 0.17;
        }
        model.neural_output_bias = 0.31;
        let features = (0..56)
            .map(|index| (index as f32 * 0.37).sin() * 8.0)
            .collect::<Vec<_>>();
        let fixed = model.score(&features);
        let generic = model.score_dynamic(&features);
        assert!((fixed - generic).abs() < 1e-4, "{fixed} != {generic}");
    }

    #[test]
    fn repeated_snapshot_does_not_advance_the_build_counter() {
        let board = queued_board();
        let mut opponent_board = queued_board();
        let mut opponent_field = [[false; 10]; 40];
        for row in opponent_field.iter_mut().take(8) {
            for cell in row.iter_mut().skip(1) {
                *cell = true;
            }
        }
        opponent_board.set_field(opponent_field);
        let observation = Observation {
            now_ms: 0,
            rules: crate::Rules::live(),
            own: view(board, 0),
            opponent: view(opponent_board, 0),
        };
        let fallback_action = legal_actions(&observation.own.board)
            .into_iter()
            .next()
            .unwrap();
        let fallback = SelectedAction {
            action: fallback_action,
            wait_ms: 0,
            intent: Intent::Stack,
            score: 0.0,
            policy_override: PolicyOverride::None,
            strategy_event: StrategyEvent::None,
            strategy_detail: 0,
        };
        let mut config = AgentConfig {
            stack_ren_enable: true,
            stack_ren_entry_interval: 1,
            stack_ren_entry_threshold: -10_000.0,
            stack_ren_max_forecast_1000: u32::MAX,
            cold_clear_nodes: 32,
            forecast_nodes: 32,
            ..AgentConfig::default()
        };
        config.stack_ren_min_fire_chain = 1;
        let base_model = BaseModel::default();
        let model = StackRenModel::bootstrap();
        let mut memory = StackRenMemory::default();
        let first = choose_stack_ren(
            &observation,
            &config,
            &base_model,
            &model,
            &fallback,
            &mut memory,
        )
        .unwrap();
        let second = choose_stack_ren(
            &observation,
            &config,
            &base_model,
            &model,
            &fallback,
            &mut memory,
        )
        .unwrap();
        assert_eq!(first.strategy_event, StrategyEvent::StackRenEnter);
        assert_eq!(second.strategy_event, StrategyEvent::StackRenEnter);
        assert_eq!(memory.build_pieces, 0);
    }

    #[test]
    fn aligned_garbage_hole_beats_a_mismatched_well() {
        let mut board = Board::new();
        let mut field = [[false; 10]; 40];
        for x in 0..10 {
            if x != 3 {
                field[0][x] = true;
            }
        }
        board.set_field(field);
        let aligned = well_metrics(&board, 3, 4);
        let mismatched = well_metrics(&board, 6, 4);
        assert!(aligned.garbage_hole_at_lowest);
        assert!(mismatched.garbage_hole_present);
        assert!(!mismatched.garbage_hole_at_lowest);
    }

    #[test]
    fn well_contamination_detects_blocks_above_its_floor() {
        let mut board = Board::new();
        let mut field = [[false; 10]; 40];
        field[0][3] = true;
        board.set_field(field);
        let metrics = well_metrics(&board, 3, 4);
        assert_eq!(metrics.floor, 0);
        assert_eq!(metrics.contamination, 1);
    }

    #[test]
    fn clean_four_wide_prefers_a_buildable_edge_platform() {
        let board = queued_board();
        let observation = Observation {
            now_ms: 0,
            rules: crate::Rules::live(),
            own: view(board.clone(), 0),
            opponent: view(board, 0),
        };
        let config = AgentConfig {
            stack_ren_min_well_width: 4,
            stack_ren_max_well_width: 4,
            ..AgentConfig::default()
        };
        let choice = choose_well(
            &observation,
            &config,
            &StackRenModel::bootstrap(),
            BoardGeometry::measure(&observation.own.board),
            BoardGeometry::measure(&observation.opponent.board),
            ForecastSummary::default(),
            0,
            0,
        )
        .unwrap();
        assert!(choice.start == 0 || choice.start + choice.width == 10);
    }

    #[test]
    fn ideal_four_wide_has_a_queue_proven_combo_route() {
        let mut board = Board::new();
        let mut field = [[false; 10]; 40];
        for row in field.iter_mut().take(5) {
            for (x, cell) in row.iter_mut().enumerate() {
                *cell = !(3..7).contains(&x);
            }
        }
        board.set_field(field);
        for _ in 0..7 {
            board.add_next_piece(Piece::I);
        }
        let observation = Observation {
            now_ms: 0,
            rules: crate::Rules::live(),
            own: view(board.clone(), 0),
            opponent: view(board, 0),
        };
        let (chain, attack) = best_ren_projection_with_limits(&observation, Some((3, 4)), 4, 16);
        assert!(chain >= 4, "chain={chain}, attack={attack}");
        assert!(attack >= 2, "chain={chain}, attack={attack}");
    }

    #[test]
    fn ren_return_is_superlinear() {
        assert!(ren_attack_estimate(12) > ren_attack_estimate(6) * 2);
    }
}
