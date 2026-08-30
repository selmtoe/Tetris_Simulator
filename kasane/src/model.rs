use anyhow::{bail, Context, Result};
use libtetris::Board;
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::fs;
use std::path::Path;

pub const FEATURE_NAMES: [&str; 44] = [
    "base_value",
    "base_spike",
    "raw_attack",
    "sent_now",
    "pressure_after_reply",
    "cancelled_own",
    "cleared_lines",
    "action_seconds",
    "wait_seconds",
    "own_height",
    "own_holes",
    "own_covered",
    "own_bumpiness",
    "own_blocks",
    "opponent_height",
    "opponent_holes",
    "opponent_covered",
    "opponent_blocks",
    "incoming_now",
    "incoming_at_lock",
    "rise_after_action",
    "safety_margin",
    "predicted_attack",
    "predicted_outgoing",
    "seconds_to_reply",
    "cancellation_dodge_gain",
    "fires_after_reply",
    "fires_before_reply",
    "fires_simultaneously",
    "tank_window",
    "kill_pressure",
    "combo",
    "back_to_back",
    "dig_delta",
    "accessible_holes",
    "perfect_clear",
    "clear_delay_seconds",
    "opponent_pending",
    "action_is_clear",
    "charge_attack",
    "charge_dodge_gain",
    "charge_pressure",
    "charge_seconds",
    "charge_feasible",
];

/// Additional policy-improvement inputs used by the v2 strategic model.  The
/// first 44 inputs intentionally remain byte-for-byte compatible with the v1
/// Tempo model.  A v1 JSON can therefore still be loaded and used by the
/// legacy policy while v2 models opt in by carrying this extended schema.
pub const STRATEGY_EXTRA_FEATURE_NAMES: [&str; 56] = [
    "baseline_same_action",
    "candidate_is_alternative",
    "base_value_delta",
    "base_spike_delta",
    "headroom_delta_vs_cc",
    "holes_delta_vs_cc",
    "covered_delta_vs_cc",
    "bumpiness_delta_vs_cc",
    "accessible_delta_vs_cc",
    "rise_delta_vs_cc",
    "cancel_delta_vs_cc",
    "sent_delta_vs_cc",
    "pressure_delta_vs_cc",
    "action_seconds_delta_vs_cc",
    "own_headroom_after_rise",
    "opponent_headroom_after_pressure",
    "lethal_overflow",
    "incoming_matured_now",
    "incoming_due_250",
    "incoming_due_500",
    "incoming_due_1000",
    "first_rise_seconds",
    "cancelled_due_500",
    "uncancelled_due_500",
    "forecast_confidence",
    "forecast_attack_500",
    "forecast_attack_1000",
    "forecast_attack_2000",
    "forecast_outgoing_500",
    "forecast_outgoing_1000",
    "forecast_outgoing_2000",
    "forecast_max_burst",
    "first_attack_seconds",
    "forecast_event_count",
    "next_action_count",
    "next_clear_count",
    "next_attack_count",
    "next_max_attack",
    "next_mean_top_attack",
    "combo_continuation_count",
    "combo_max_attack",
    "ren_length",
    "dense_rows_after",
    "hole_burden",
    "attack_per_second",
    "pressure_per_second",
    "initiative_seconds",
    "synchronized_pressure",
    "resource_balance_blocks",
    "opponent_dig_burden",
    "own_dig_burden",
    "survival_risk",
    "wait_fraction",
    "timing_slack_seconds",
    "baseline_raw_attack",
    "baseline_sent",
];

pub const STRATEGY_STATE_FEATURE_NAMES: [&str; 16] = [
    "phase_charge",
    "phase_armed",
    "charge_pieces",
    "charge_state_seconds",
    "immediate_attack_options",
    "safe_nonfire_options",
    "max_immediate_attack",
    "incoming_delta_observed",
    "release_trigger",
    "opponent_attack_eta",
    "charge_safety_score",
    "accumulated_resource_gain",
    "next_i_distance",
    "next_t_distance",
    "hold_is_i_or_t",
    "ren_continuation_potential",
];

pub fn strategy_feature_names() -> Vec<String> {
    FEATURE_NAMES
        .iter()
        .chain(STRATEGY_EXTRA_FEATURE_NAMES.iter())
        .chain(STRATEGY_STATE_FEATURE_NAMES.iter())
        .map(|name| (*name).to_owned())
        .collect()
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DenseLayer {
    pub weights: Vec<Vec<f32>>,
    pub bias: Vec<f32>,
}

/// A residual tactical network. The linear path makes a model inspectable at
/// bootstrap; trained hidden layers can then learn interactions such as
/// `opponent high AND fire after cancellation` without replacing that prior.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TempoModel {
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

impl Default for TempoModel {
    fn default() -> Self {
        let names = FEATURE_NAMES
            .iter()
            .map(|name| (*name).to_owned())
            .collect();
        let mut scale = vec![1.0; FEATURE_NAMES.len()];
        let mut weight = vec![0.0; FEATURE_NAMES.len()];
        let mut set = |name: &str, divisor: f32, value: f32| {
            let index = FEATURE_NAMES.iter().position(|item| *item == name).unwrap();
            scale[index] = divisor;
            weight[index] = value;
        };

        // KASANE-Base remains the geometric prior. The larger tactical terms
        // model packet survival, timing, tanking and opponent-dependent kill
        // pressure that are absent from the stacking evaluator.
        set("base_value", 10.0, 1.35);
        set("base_spike", 10.0, 0.30);
        set("raw_attack", 4.0, 0.45);
        set("sent_now", 4.0, 1.15);
        set("pressure_after_reply", 4.0, 3.50);
        set("cancelled_own", 4.0, -0.35);
        set("cleared_lines", 4.0, 0.12);
        set("action_seconds", 1.0, -0.25);
        set("wait_seconds", 1.0, -0.18);
        set("own_height", 10.0, -1.10);
        set("own_holes", 5.0, -2.20);
        set("own_covered", 10.0, -0.85);
        set("own_bumpiness", 10.0, -0.28);
        set("own_blocks", 100.0, 0.06);
        set("opponent_height", 10.0, 0.45);
        set("opponent_holes", 5.0, 0.30);
        set("opponent_covered", 10.0, 0.18);
        set("opponent_blocks", 100.0, 0.10);
        set("incoming_now", 4.0, -0.24);
        set("incoming_at_lock", 4.0, -0.32);
        set("rise_after_action", 4.0, -5.00);
        set("safety_margin", 10.0, 1.05);
        set("predicted_attack", 4.0, -0.10);
        set("predicted_outgoing", 4.0, -0.48);
        set("seconds_to_reply", 1.0, 0.04);
        set("cancellation_dodge_gain", 4.0, 5.50);
        set("fires_after_reply", 1.0, -0.20);
        set("fires_before_reply", 1.0, -0.12);
        set("fires_simultaneously", 1.0, 2.00);
        set("tank_window", 1.0, 0.25);
        set("kill_pressure", 4.0, 1.70);
        set("combo", 4.0, 0.28);
        set("back_to_back", 1.0, 0.16);
        set("dig_delta", 4.0, 0.75);
        set("accessible_holes", 4.0, 0.34);
        set("perfect_clear", 1.0, 0.0); // Explicit PC0 contract.
        set("clear_delay_seconds", 1.0, -0.18);
        set("opponent_pending", 4.0, 0.14);
        set("action_is_clear", 1.0, 0.05);
        set("charge_attack", 4.0, 0.30);
        set("charge_dodge_gain", 4.0, 5.00);
        set("charge_pressure", 4.0, 2.20);
        set("charge_seconds", 1.0, -0.35);
        set("charge_feasible", 1.0, 1.25);

        Self {
            schema: "kasane-tempo-model/v1".to_owned(),
            feature_names: names,
            input_mean: vec![0.0; FEATURE_NAMES.len()],
            input_scale: scale,
            linear_weights: weight,
            linear_bias: 0.0,
            hidden_layers: Vec::new(),
            neural_output_weights: Vec::new(),
            neural_output_bias: 0.0,
        }
    }
}

impl TempoModel {
    /// Construct the v2 residual strategy network.  Cold Clear remains the
    /// policy floor; this network only scores timing changes and safety-gated
    /// alternatives relative to that floor.  Hidden projections are fixed
    /// deterministic random features so CEM/ES can train the compact output
    /// genome without evaluating thousands of independent parameters.
    pub fn strategy_v2_bootstrap() -> Self {
        let legacy = Self::default();
        let mut model = Self {
            schema: "kasane-strategy-model/v2-cc-floor-mlp".to_owned(),
            feature_names: strategy_feature_names(),
            input_mean: legacy.input_mean,
            input_scale: legacy.input_scale,
            linear_weights: legacy.linear_weights,
            linear_bias: legacy.linear_bias,
            hidden_layers: Vec::new(),
            neural_output_weights: Vec::new(),
            neural_output_bias: 0.0,
        };

        let extras: [(&str, f32, f32); 56] = [
            ("baseline_same_action", 1.0, 0.10),
            ("candidate_is_alternative", 1.0, -0.35),
            ("base_value_delta", 4.0, 0.20),
            ("base_spike_delta", 4.0, 0.12),
            ("headroom_delta_vs_cc", 4.0, 0.90),
            ("holes_delta_vs_cc", 3.0, -1.10),
            ("covered_delta_vs_cc", 5.0, -0.70),
            ("bumpiness_delta_vs_cc", 6.0, -0.18),
            ("accessible_delta_vs_cc", 3.0, 0.28),
            ("rise_delta_vs_cc", 4.0, -1.60),
            ("cancel_delta_vs_cc", 4.0, 1.20),
            ("sent_delta_vs_cc", 4.0, 0.75),
            ("pressure_delta_vs_cc", 4.0, 1.35),
            ("action_seconds_delta_vs_cc", 1.0, -0.35),
            ("own_headroom_after_rise", 10.0, 0.80),
            ("opponent_headroom_after_pressure", 10.0, -0.70),
            ("lethal_overflow", 4.0, 2.20),
            ("incoming_matured_now", 4.0, -1.00),
            ("incoming_due_250", 4.0, -0.90),
            ("incoming_due_500", 4.0, -0.65),
            ("incoming_due_1000", 4.0, -0.35),
            ("first_rise_seconds", 1.0, 0.20),
            ("cancelled_due_500", 4.0, 0.85),
            ("uncancelled_due_500", 4.0, -1.10),
            ("forecast_confidence", 1.0, 0.08),
            ("forecast_attack_500", 4.0, -0.10),
            ("forecast_attack_1000", 4.0, -0.08),
            ("forecast_attack_2000", 8.0, -0.04),
            ("forecast_outgoing_500", 4.0, -0.22),
            ("forecast_outgoing_1000", 4.0, -0.15),
            ("forecast_outgoing_2000", 8.0, -0.08),
            ("forecast_max_burst", 6.0, -0.16),
            ("first_attack_seconds", 1.0, 0.05),
            ("forecast_event_count", 4.0, 0.02),
            ("next_action_count", 48.0, 0.08),
            ("next_clear_count", 12.0, 0.14),
            ("next_attack_count", 10.0, 0.20),
            ("next_max_attack", 6.0, 0.32),
            ("next_mean_top_attack", 4.0, 0.18),
            ("combo_continuation_count", 10.0, 0.38),
            ("combo_max_attack", 6.0, 0.52),
            ("ren_length", 6.0, 0.30),
            ("dense_rows_after", 8.0, -0.08),
            ("hole_burden", 10.0, -0.70),
            ("attack_per_second", 8.0, 0.32),
            ("pressure_per_second", 8.0, 0.48),
            ("initiative_seconds", 1.0, -0.08),
            ("synchronized_pressure", 6.0, 1.20),
            ("resource_balance_blocks", 50.0, 0.08),
            ("opponent_dig_burden", 10.0, 0.18),
            ("own_dig_burden", 10.0, -0.42),
            ("survival_risk", 6.0, -1.60),
            ("wait_fraction", 1.0, -0.20),
            ("timing_slack_seconds", 1.0, 0.06),
            ("baseline_raw_attack", 4.0, 0.04),
            ("baseline_sent", 4.0, 0.03),
        ];
        for (name, scale, weight) in extras {
            debug_assert_eq!(model.feature_names[model.input_scale.len()], name);
            model.input_mean.push(0.0);
            model.input_scale.push(scale);
            model.linear_weights.push(weight);
        }
        let state_features: [(&str, f32, f32); 16] = [
            ("phase_charge", 1.0, 0.16),
            ("phase_armed", 1.0, 0.24),
            ("charge_pieces", 6.0, 0.12),
            ("charge_state_seconds", 3.0, -0.08),
            ("immediate_attack_options", 8.0, 0.22),
            ("safe_nonfire_options", 12.0, 0.10),
            ("max_immediate_attack", 6.0, 0.38),
            ("incoming_delta_observed", 4.0, 0.70),
            ("release_trigger", 1.0, 0.70),
            ("opponent_attack_eta", 1.0, -0.05),
            ("charge_safety_score", 10.0, 0.30),
            ("accumulated_resource_gain", 8.0, 0.34),
            ("next_i_distance", 7.0, -0.06),
            ("next_t_distance", 7.0, -0.05),
            ("hold_is_i_or_t", 1.0, 0.12),
            ("ren_continuation_potential", 10.0, 0.38),
        ];
        for (name, scale, weight) in state_features {
            debug_assert_eq!(model.feature_names[model.input_scale.len()], name);
            model.input_mean.push(0.0);
            model.input_scale.push(scale);
            model.linear_weights.push(weight);
        }

        let width = model.feature_names.len();
        let pc_index = model
            .feature_names
            .iter()
            .position(|name| name == "perfect_clear")
            .expect("PC feature is part of the legacy prefix");
        let first_width = 64;
        let mut first = DenseLayer {
            weights: vec![vec![0.0; width]; first_width],
            bias: vec![0.0; first_width],
        };
        let mut state = 0x4B41_5341_4E45_0002_u64;
        for unit in 0..first_width {
            first.bias[unit] = if unit % 3 == 0 { -0.15 } else { 0.0 };
            for _ in 0..10 {
                state = splitmix64(state);
                let index = (state as usize) % width;
                if index == pc_index {
                    continue;
                }
                state = splitmix64(state);
                let magnitude = 0.08 + ((state >> 48) as f32 / u16::MAX as f32) * 0.22;
                let sign = if state & 1 == 0 { -1.0 } else { 1.0 };
                first.weights[unit][index] += sign * magnitude;
            }
        }
        let second_width = 32;
        let mut second = DenseLayer {
            weights: vec![vec![0.0; first_width]; second_width],
            bias: vec![0.0; second_width],
        };
        for unit in 0..second_width {
            for input in 0..first_width {
                state = splitmix64(state);
                let centered = ((state >> 48) as f32 / u16::MAX as f32) * 2.0 - 1.0;
                second.weights[unit][input] = centered * 0.16;
            }
        }
        model.hidden_layers = vec![first, second];
        model.neural_output_weights = vec![0.0; second_width];
        model
    }

    pub fn is_strategy_v2(&self) -> bool {
        self.schema.starts_with("kasane-strategy-model/v2")
            && self.feature_names == strategy_feature_names()
    }

    pub fn load(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref();
        let data = fs::read(path).with_context(|| format!("failed to read {}", path.display()))?;
        let model: Self = serde_json::from_slice(&data)
            .with_context(|| format!("failed to parse {}", path.display()))?;
        model.validate()?;
        Ok(model)
    }

    pub fn save(&self, path: impl AsRef<Path>) -> Result<()> {
        self.validate()?;
        let bytes = serde_json::to_vec_pretty(self)?;
        fs::write(path, bytes)?;
        Ok(())
    }

    pub fn validate(&self) -> Result<()> {
        let legacy_names: Vec<String> = FEATURE_NAMES
            .iter()
            .map(|name| (*name).to_owned())
            .collect();
        let strategy_names = strategy_feature_names();
        if self.feature_names != legacy_names && self.feature_names != strategy_names {
            bail!("tempo model feature schema does not match this KASANE build");
        }
        let width = self.feature_names.len();
        if self.input_mean.len() != width
            || self.input_scale.len() != width
            || self.linear_weights.len() != width
        {
            bail!("tempo model input dimensions are invalid");
        }
        if self
            .input_scale
            .iter()
            .any(|value| !value.is_finite() || *value <= 0.0)
        {
            bail!("tempo model scales must be finite and positive");
        }
        let mut input_width = width;
        for layer in &self.hidden_layers {
            if layer.bias.len() != layer.weights.len()
                || layer.weights.iter().any(|row| row.len() != input_width)
            {
                bail!("tempo model hidden layer dimensions are invalid");
            }
            input_width = layer.bias.len();
        }
        if !self.hidden_layers.is_empty() && self.neural_output_weights.len() != input_width {
            bail!("tempo model output dimensions are invalid");
        }
        Ok(())
    }

    pub fn score(&self, raw: &[f32]) -> f32 {
        debug_assert_eq!(raw.len(), self.feature_names.len());
        let normalized: Vec<f32> = raw
            .iter()
            .zip(&self.input_mean)
            .zip(&self.input_scale)
            .map(|((&value, &mean), &scale)| (value - mean) / scale)
            .collect();
        let mut score = self.linear_bias + dot(&self.linear_weights, &normalized);

        if !self.hidden_layers.is_empty() {
            let mut activations = normalized;
            for layer in &self.hidden_layers {
                activations = layer
                    .weights
                    .iter()
                    .zip(&layer.bias)
                    .map(|(weights, bias)| (dot(weights, &activations) + bias).max(0.0))
                    .collect();
            }
            score += self.neural_output_bias + dot(&self.neural_output_weights, &activations);
        }
        score
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

#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize)]
pub struct BoardGeometry {
    pub max_height: u32,
    pub holes: u32,
    pub covered: u32,
    pub bumpiness: u32,
    pub blocks: u32,
    pub bottom_dense_rows: u32,
    pub accessible_holes: u32,
}

impl BoardGeometry {
    pub fn measure(board: &Board) -> Self {
        let heights = board.column_heights();
        let max_height = (*heights.iter().max().unwrap_or(&0)).max(0) as u32;
        let bumpiness = heights
            .windows(2)
            .map(|pair| (pair[0] - pair[1]).unsigned_abs())
            .sum();
        let mut blocks = 0;
        let mut holes = 0;
        let mut covered = 0;
        for x in 0..10 {
            let height = heights[x].max(0);
            let mut first_hole = None;
            for y in 0..height {
                if board.occupied(x as i32, y) {
                    blocks += 1;
                } else {
                    holes += 1;
                    first_hole.get_or_insert(y);
                }
            }
            if let Some(y) = first_hole {
                covered += (y + 1..height)
                    .filter(|&row| board.occupied(x as i32, row))
                    .count() as u32;
            }
        }

        let mut bottom_dense_rows = 0;
        for y in 0..40 {
            let occupied = (0..10).filter(|&x| board.occupied(x, y)).count();
            if occupied >= 8 {
                bottom_dense_rows += 1;
            } else {
                break;
            }
        }

        Self {
            max_height,
            holes,
            covered,
            bumpiness,
            blocks,
            bottom_dense_rows,
            accessible_holes: count_sky_accessible_holes(board, max_height as usize),
        }
    }
}

fn count_sky_accessible_holes(board: &Board, max_height: usize) -> u32 {
    if max_height == 0 {
        return 0;
    }
    let ceiling = (max_height + 1).min(40);
    let mut seen = [[false; 40]; 10];
    let mut queue = VecDeque::new();
    for x in 0..10 {
        let y = ceiling - 1;
        if !board.occupied(x as i32, y as i32) {
            seen[x][y] = true;
            queue.push_back((x, y));
        }
    }
    while let Some((x, y)) = queue.pop_front() {
        for (nx, ny) in [
            (x.wrapping_sub(1), y),
            (x + 1, y),
            (x, y.wrapping_sub(1)),
            (x, y + 1),
        ] {
            if nx < 10 && ny < ceiling && !seen[nx][ny] && !board.occupied(nx as i32, ny as i32) {
                seen[nx][ny] = true;
                queue.push_back((nx, ny));
            }
        }
    }
    let mut count = 0;
    for x in 0..10 {
        for y in 0..board.column_heights()[x].max(0) as usize {
            if seen[x][y] && !board.occupied(x as i32, y as i32) {
                count += 1;
            }
        }
    }
    count
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_model_is_valid_and_pc_has_zero_weight() {
        let model = TempoModel::default();
        model.validate().unwrap();
        let pc = FEATURE_NAMES
            .iter()
            .position(|name| *name == "perfect_clear")
            .unwrap();
        assert_eq!(model.linear_weights[pc], 0.0);
        assert!(model.score(&vec![0.0; FEATURE_NAMES.len()]).is_finite());
    }
}
