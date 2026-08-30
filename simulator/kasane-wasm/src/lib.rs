//! Raw browser ABI for the KASANE Base + Tempo controller.
//!
//! The simulator sends one complete two-player observation as JSON.  This
//! wrapper only converts coordinate systems and owns the embedded, versioned
//! model artifacts; all placement and tactical decisions remain in `kasane`.

#![allow(clippy::missing_safety_doc)]

use kasane::agent::{
    AgentConfig, AgentController, AgentKind, IncomingPacket, Intent, Observation, PhaseView,
    PlayerView, PolicyOverride, SelectedAction, StackRenModel, StrategyEvent,
};
use kasane::base::BaseModel;
use kasane::model::TempoModel;
use kasane::Rules;
use libtetris::{Board, Piece, RotationState, TspinStatus};
use serde::Deserialize;
use std::cell::RefCell;
use std::slice;
use std::sync::OnceLock;

const BASE_MODEL_JSON: &str = include_str!("../../../kasane/models/base-model-evolved-v3.json");
const TEMPO_MODEL_JSON: &str = include_str!("../../../kasane/config/tempo-model-bootstrap-v3.json");
const GUARD_MODEL_JSON: &str = include_str!("../../../kasane/models/guard-model-evolved-v1.json");
const GUARD_POLICY_JSON: &str = include_str!("../../../kasane/config/kasane-guard-v1.json");
const STRATEGY_MODEL_JSON: &str = include_str!("../../../kasane/models/strategy-model-v2.json");
const STRATEGY_POLICY_JSON: &str = include_str!("../../../kasane/config/kasane-strategy-v2.json");
const STACK_REN_MODEL_JSON: &str = include_str!("../../../kasane/models/stack-ren-model-v3.json");
const STACK_REN_POLICY_JSON: &str = include_str!("../../../kasane/config/kasane-stack-ren-v3.json");
const MIN_BROWSER_NODE_LIMIT: u32 = 32;
const MAX_BROWSER_NODE_LIMIT: u32 = 200_000;

#[derive(Clone, Debug, Deserialize)]
struct GuardSafetyGate {
    max_headroom_loss: i32,
    max_hole_increase: u32,
    max_covered_increase: u32,
    max_bumpiness_increase: u32,
    max_accessible_hole_loss: u32,
    minimum_cancel_gain: u32,
}

#[derive(Clone, Debug, Deserialize)]
struct GuardPolicy {
    base_guard_margin: f32,
    base_depth: usize,
    base_beam_width: usize,
    forecast_nodes: u32,
    maximum_wait_ms: u64,
    #[serde(default = "default_guard_safety_gate")]
    safety_gate: GuardSafetyGate,
}

#[derive(Clone, Debug, Deserialize)]
struct StrategyPolicy {
    cold_clear_floor_nodes: u32,
    forecast_nodes: u32,
    maximum_wait_ms: u64,
    enable_tank: bool,
    strategy_min_advantage: f32,
    strategy_placement_min_advantage: f32,
    strategy_max_alternatives: usize,
    strategy_max_wait_ms: u64,
    strategy_enable_stateful: bool,
    strategy_charge_min_pieces: u32,
    strategy_charge_max_pieces: u32,
    strategy_charge_min_headroom: i32,
    strategy_release_window_ms: u64,
    strategy_counter_offset_ms: u64,
    strategy_enable_moe: bool,
    strategy_attack_enter_height: u32,
    strategy_attack_hard_finish_height: u32,
    strategy_attack_stay_height: u32,
    strategy_attack_hole_burden: u32,
    strategy_attack_min_headroom: i32,
    strategy_attack_max_due_1000: u32,
    strategy_enable_ren: bool,
    strategy_ren_depth: usize,
    strategy_ren_beam_width: usize,
    strategy_ren_start_chain: u32,
    strategy_ren_min_headroom: i32,
}

#[derive(Clone, Debug, Deserialize)]
struct StackRenPolicy {
    stack_ren_enable: bool,
    stack_ren_min_well_width: usize,
    stack_ren_max_well_width: usize,
    stack_ren_entry_interval: u32,
    stack_ren_action_limit: usize,
    stack_ren_min_build_pieces: u32,
    stack_ren_max_build_pieces: u32,
    stack_ren_max_build_ms: u64,
    stack_ren_target_depth: u32,
    stack_ren_min_fire_chain: u32,
    stack_ren_min_fire_attack: u32,
    stack_ren_min_headroom: i32,
    stack_ren_max_holes: u32,
    stack_ren_max_due_1000: u32,
    stack_ren_max_forecast_1000: u32,
    stack_ren_entry_threshold: f32,
    stack_ren_fire_margin: f32,
    stack_ren_abort_margin: f32,
    stack_ren_hole_mismatch_penalty: f32,
    stack_ren_cooldown_pieces: u32,
}

fn default_guard_safety_gate() -> GuardSafetyGate {
    GuardSafetyGate {
        max_headroom_loss: 0,
        max_hole_increase: 0,
        max_covered_increase: 0,
        max_bumpiness_increase: 2,
        max_accessible_hole_loss: 0,
        minimum_cancel_gain: 1,
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserPacket {
    lines: u32,
    arrival_ms: u64,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserPhase {
    #[serde(default)]
    kind: String,
    #[serde(default)]
    started_ms: u64,
    #[serde(default)]
    ends_ms: u64,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserPlayer {
    board: Vec<u8>,
    current_piece: String,
    #[serde(default)]
    next_queue: Vec<String>,
    #[serde(default)]
    hold_piece: Option<String>,
    #[serde(default = "default_true")]
    can_hold: bool,
    #[serde(default)]
    is_b2_b: bool,
    #[serde(default = "default_ren")]
    ren: i32,
    #[serde(default)]
    incoming: Vec<BrowserPacket>,
    #[serde(default)]
    phase: BrowserPhase,
    #[serde(default)]
    pieces: u64,
    #[serde(default = "default_piece_ms")]
    average_piece_ms: f32,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(default, rename_all = "camelCase")]
struct BrowserRules {
    input_interval_ms: u64,
    line_clear_delay_ms: u64,
    garbage_grace_ms: u64,
    decision_latency_ms: u64,
    preview_count: usize,
    garbage_randomness: f64,
    perfect_clear_special_attack: u32,
    spawn_delay_ms: u64,
}

impl Default for BrowserRules {
    fn default() -> Self {
        let rules = Rules::pinned();
        Self {
            input_interval_ms: rules.input_interval_ms,
            line_clear_delay_ms: rules.line_clear_delay_ms,
            garbage_grace_ms: rules.garbage_grace_ms,
            decision_latency_ms: rules.decision_latency_ms,
            preview_count: rules.preview_count,
            garbage_randomness: rules.garbage_randomness,
            perfect_clear_special_attack: 10,
            spawn_delay_ms: rules.spawn_delay_ms,
        }
    }
}

impl BrowserRules {
    fn into_rules(self) -> Rules {
        Rules {
            schema: "kasane-rules/browser-live-v1".to_owned(),
            input_interval_ms: self.input_interval_ms.max(1),
            line_clear_delay_ms: self.line_clear_delay_ms,
            garbage_grace_ms: self.garbage_grace_ms,
            decision_latency_ms: self.decision_latency_ms,
            preview_count: self.preview_count.max(1),
            garbage_randomness: self.garbage_randomness.clamp(0.0, 1.0),
            perfect_clear_special_attack: self.perfect_clear_special_attack,
            spawn_delay_ms: self.spawn_delay_ms,
        }
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserSnapshot {
    now_ms: u64,
    own: BrowserPlayer,
    opponent: BrowserPlayer,
    #[serde(default)]
    has_opponent: bool,
    #[serde(default = "default_model")]
    model: String,
    #[serde(default)]
    rules: BrowserRules,
    /// Optional live-search controls. Older workers omit both fields and keep
    /// the exact v1 search settings. Newer workers can pass the simulator's
    /// UI budgets without changing the raw function ABI.
    /// Search compute budget only. This is deliberately independent from
    /// `rules.decisionLatencyMs`, which remains part of tactical lock timing.
    /// `thinkTimeMs` is accepted as a migration alias for the parent worker.
    #[serde(default, alias = "thinkTimeMs")]
    search_think_time_ms: Option<u64>,
    #[serde(default)]
    node_limit: Option<u32>,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
enum SearchTier {
    Economy,
    Standard,
    Enhanced,
    Maximum,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
struct SearchProfile {
    base_depth: usize,
    base_beam_width: usize,
    forecast_nodes: u32,
    ren_depth: usize,
    ren_beam_width: usize,
    stack_action_limit: usize,
}

impl SearchTier {
    fn for_think_time(think_time_ms: u64) -> Self {
        match think_time_ms {
            0..=16 => Self::Economy,
            17..=35 => Self::Standard,
            36..=75 => Self::Enhanced,
            _ => Self::Maximum,
        }
    }

    fn for_node_limit(node_limit: u32) -> Self {
        match node_limit {
            0..=10_000 => Self::Economy,
            10_001..=50_000 => Self::Standard,
            50_001..=150_000 => Self::Enhanced,
            _ => Self::Maximum,
        }
    }

    fn profile(self, is_guard: bool) -> SearchProfile {
        match (is_guard, self) {
            (false, Self::Economy) => SearchProfile {
                base_depth: 3,
                base_beam_width: 24,
                forecast_nodes: 96,
                ren_depth: 4,
                ren_beam_width: 10,
                stack_action_limit: 24,
            },
            (false, Self::Standard) => SearchProfile {
                base_depth: 4,
                base_beam_width: 48,
                forecast_nodes: 240,
                ren_depth: 5,
                ren_beam_width: 14,
                stack_action_limit: 32,
            },
            (false, Self::Enhanced) => SearchProfile {
                base_depth: 4,
                base_beam_width: 80,
                forecast_nodes: 600,
                ren_depth: 6,
                ren_beam_width: 20,
                stack_action_limit: 48,
            },
            (false, Self::Maximum) => SearchProfile {
                base_depth: 5,
                base_beam_width: 112,
                forecast_nodes: 1_200,
                ren_depth: 7,
                ren_beam_width: 28,
                stack_action_limit: 64,
            },
            (true, Self::Economy) => SearchProfile {
                base_depth: 2,
                base_beam_width: 12,
                forecast_nodes: 40,
                ren_depth: 4,
                ren_beam_width: 10,
                stack_action_limit: 24,
            },
            (true, Self::Standard) => SearchProfile {
                base_depth: 3,
                base_beam_width: 24,
                forecast_nodes: 128,
                ren_depth: 5,
                ren_beam_width: 14,
                stack_action_limit: 32,
            },
            (true, Self::Enhanced) => SearchProfile {
                base_depth: 3,
                base_beam_width: 48,
                forecast_nodes: 320,
                ren_depth: 6,
                ren_beam_width: 20,
                stack_action_limit: 48,
            },
            (true, Self::Maximum) => SearchProfile {
                base_depth: 4,
                base_beam_width: 72,
                forecast_nodes: 800,
                ren_depth: 7,
                ren_beam_width: 28,
                stack_action_limit: 64,
            },
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct ControllerKey {
    kind: AgentKind,
    cold_clear_nodes: u32,
    kasane_nodes: u32,
    forecast_nodes: u32,
    maximum_wait_ms: u64,
    enable_tank: bool,
    dodge_wait_cap_ms: u64,
    dodge_finish_height: u32,
    dodge_safety_margin: i32,
    enable_charge: bool,
    strict_base_policy: bool,
    base_depth: usize,
    base_beam_width: usize,
    enable_tempo: bool,
    base_guard_margin_bits: u32,
    guard_max_headroom_loss: i32,
    guard_max_hole_increase: u32,
    guard_max_covered_increase: u32,
    guard_max_bumpiness_increase: u32,
    guard_max_accessible_hole_loss: u32,
    guard_minimum_cancel_gain: u32,
    strategy_min_advantage_bits: u32,
    strategy_placement_min_advantage_bits: u32,
    strategy_max_alternatives: usize,
    strategy_max_wait_ms: u64,
    strategy_enable_stateful: bool,
    strategy_charge_min_pieces: u32,
    strategy_charge_max_pieces: u32,
    strategy_charge_min_headroom: i32,
    strategy_release_window_ms: u64,
    strategy_counter_offset_ms: u64,
    strategy_enable_moe: bool,
    strategy_attack_enter_height: u32,
    strategy_attack_hard_finish_height: u32,
    strategy_attack_stay_height: u32,
    strategy_attack_hole_burden: u32,
    strategy_attack_min_headroom: i32,
    strategy_attack_max_due_1000: u32,
    strategy_enable_ren: bool,
    strategy_ren_depth: usize,
    strategy_ren_beam_width: usize,
    strategy_ren_start_chain: u32,
    strategy_ren_min_headroom: i32,
    stack_ren_enable: bool,
    stack_ren_min_well_width: usize,
    stack_ren_max_well_width: usize,
    stack_ren_entry_interval: u32,
    stack_ren_action_limit: usize,
    stack_ren_min_build_pieces: u32,
    stack_ren_max_build_pieces: u32,
    stack_ren_max_build_ms: u64,
    stack_ren_target_depth: u32,
    stack_ren_min_fire_chain: u32,
    stack_ren_min_fire_attack: u32,
    stack_ren_min_headroom: i32,
    stack_ren_max_holes: u32,
    stack_ren_max_due_1000: u32,
    stack_ren_max_forecast_1000: u32,
    stack_ren_entry_threshold_bits: u32,
    stack_ren_fire_margin_bits: u32,
    stack_ren_abort_margin_bits: u32,
    stack_ren_hole_mismatch_penalty_bits: u32,
    stack_ren_cooldown_pieces: u32,
}

impl ControllerKey {
    fn new(kind: AgentKind, config: &AgentConfig) -> Self {
        Self {
            kind,
            cold_clear_nodes: config.cold_clear_nodes,
            kasane_nodes: config.kasane_nodes,
            forecast_nodes: config.forecast_nodes,
            maximum_wait_ms: config.maximum_wait_ms,
            enable_tank: config.enable_tank,
            dodge_wait_cap_ms: config.dodge_wait_cap_ms,
            dodge_finish_height: config.dodge_finish_height,
            dodge_safety_margin: config.dodge_safety_margin,
            enable_charge: config.enable_charge,
            strict_base_policy: config.strict_base_policy,
            base_depth: config.base_depth,
            base_beam_width: config.base_beam_width,
            enable_tempo: config.enable_tempo,
            base_guard_margin_bits: config.base_guard_margin.to_bits(),
            guard_max_headroom_loss: config.guard_max_headroom_loss,
            guard_max_hole_increase: config.guard_max_hole_increase,
            guard_max_covered_increase: config.guard_max_covered_increase,
            guard_max_bumpiness_increase: config.guard_max_bumpiness_increase,
            guard_max_accessible_hole_loss: config.guard_max_accessible_hole_loss,
            guard_minimum_cancel_gain: config.guard_minimum_cancel_gain,
            strategy_min_advantage_bits: config.strategy_min_advantage.to_bits(),
            strategy_placement_min_advantage_bits: config
                .strategy_placement_min_advantage
                .to_bits(),
            strategy_max_alternatives: config.strategy_max_alternatives,
            strategy_max_wait_ms: config.strategy_max_wait_ms,
            strategy_enable_stateful: config.strategy_enable_stateful,
            strategy_charge_min_pieces: config.strategy_charge_min_pieces,
            strategy_charge_max_pieces: config.strategy_charge_max_pieces,
            strategy_charge_min_headroom: config.strategy_charge_min_headroom,
            strategy_release_window_ms: config.strategy_release_window_ms,
            strategy_counter_offset_ms: config.strategy_counter_offset_ms,
            strategy_enable_moe: config.strategy_enable_moe,
            strategy_attack_enter_height: config.strategy_attack_enter_height,
            strategy_attack_hard_finish_height: config.strategy_attack_hard_finish_height,
            strategy_attack_stay_height: config.strategy_attack_stay_height,
            strategy_attack_hole_burden: config.strategy_attack_hole_burden,
            strategy_attack_min_headroom: config.strategy_attack_min_headroom,
            strategy_attack_max_due_1000: config.strategy_attack_max_due_1000,
            strategy_enable_ren: config.strategy_enable_ren,
            strategy_ren_depth: config.strategy_ren_depth,
            strategy_ren_beam_width: config.strategy_ren_beam_width,
            strategy_ren_start_chain: config.strategy_ren_start_chain,
            strategy_ren_min_headroom: config.strategy_ren_min_headroom,
            stack_ren_enable: config.stack_ren_enable,
            stack_ren_min_well_width: config.stack_ren_min_well_width,
            stack_ren_max_well_width: config.stack_ren_max_well_width,
            stack_ren_entry_interval: config.stack_ren_entry_interval,
            stack_ren_action_limit: config.stack_ren_action_limit,
            stack_ren_min_build_pieces: config.stack_ren_min_build_pieces,
            stack_ren_max_build_pieces: config.stack_ren_max_build_pieces,
            stack_ren_max_build_ms: config.stack_ren_max_build_ms,
            stack_ren_target_depth: config.stack_ren_target_depth,
            stack_ren_min_fire_chain: config.stack_ren_min_fire_chain,
            stack_ren_min_fire_attack: config.stack_ren_min_fire_attack,
            stack_ren_min_headroom: config.stack_ren_min_headroom,
            stack_ren_max_holes: config.stack_ren_max_holes,
            stack_ren_max_due_1000: config.stack_ren_max_due_1000,
            stack_ren_max_forecast_1000: config.stack_ren_max_forecast_1000,
            stack_ren_entry_threshold_bits: config.stack_ren_entry_threshold.to_bits(),
            stack_ren_fire_margin_bits: config.stack_ren_fire_margin.to_bits(),
            stack_ren_abort_margin_bits: config.stack_ren_abort_margin.to_bits(),
            stack_ren_hole_mismatch_penalty_bits: config.stack_ren_hole_mismatch_penalty.to_bits(),
            stack_ren_cooldown_pieces: config.stack_ren_cooldown_pieces,
        }
    }
}

struct CachedController {
    model_id: String,
    key: ControllerKey,
    controller: AgentController,
}

#[derive(Default)]
struct ControllerCache {
    current: Option<CachedController>,
    builds: u32,
    hits: u32,
}

impl ControllerCache {
    fn ensure(&mut self, model_id: &str, kind: AgentKind, mut config: AgentConfig) -> bool {
        let key = ControllerKey::new(kind, &config);
        if self
            .current
            .as_ref()
            .is_some_and(|entry| entry.model_id == model_id && entry.key == key)
        {
            self.hits = self.hits.saturating_add(1);
            return true;
        }

        config.base_model_override = Some(base_model().clone());
        config.tempo_model_override = Some(match model_id {
            "kasane-strategy" | "kasane-stack-ren" => strategy_model().clone(),
            _ if kind == AgentKind::KasaneGuard => guard_model().clone(),
            _ => tempo_model().clone(),
        });
        if model_id == "kasane-stack-ren" {
            config.stack_ren_model_override = Some(stack_ren_model().clone());
        }
        let Ok(controller) = AgentController::new(kind, config) else {
            self.current = None;
            return false;
        };
        self.current = Some(CachedController {
            model_id: model_id.to_owned(),
            key,
            controller,
        });
        self.builds = self.builds.saturating_add(1);
        true
    }
}

thread_local! {
    /// A Web Worker owns one WASM instance and calls it synchronously, making
    /// thread-local interior mutability sufficient without locks or `static
    /// mut`. The key prevents stale settings from surviving a UI change.
    static CONTROLLER_CACHE: RefCell<ControllerCache> = RefCell::new(ControllerCache::default());
}

fn default_ren() -> i32 {
    -1
}

fn default_piece_ms() -> f32 {
    350.0
}

fn default_true() -> bool {
    true
}

fn default_model() -> String {
    "kasane-basic".to_owned()
}

#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct KasaneMove {
    pub status: u32,
    pub piece: u8,
    pub hold: u8,
    pub rotation: u8,
    pub tspin: u8,
    pub x: i32,
    pub y: i32,
    pub wait_ms: u32,
    pub score: f32,
    pub intent: u8,
    pub reserved: [u8; 3],
    pub attack: u32,
}

impl Default for KasaneMove {
    fn default() -> Self {
        Self {
            status: 0,
            piece: 0,
            hold: 0,
            rotation: 0,
            tspin: 0,
            x: 0,
            y: 0,
            wait_ms: 0,
            score: 0.0,
            intent: 0,
            reserved: [0; 3],
            attack: 0,
        }
    }
}

fn base_model() -> &'static BaseModel {
    static MODEL: OnceLock<BaseModel> = OnceLock::new();
    MODEL.get_or_init(|| {
        serde_json::from_str(BASE_MODEL_JSON).expect("embedded KASANE Base model must be valid")
    })
}

fn tempo_model() -> &'static TempoModel {
    static MODEL: OnceLock<TempoModel> = OnceLock::new();
    MODEL.get_or_init(|| {
        serde_json::from_str(TEMPO_MODEL_JSON).expect("embedded KASANE Tempo model must be valid")
    })
}

fn guard_model() -> &'static TempoModel {
    static MODEL: OnceLock<TempoModel> = OnceLock::new();
    MODEL.get_or_init(|| {
        serde_json::from_str(GUARD_MODEL_JSON).expect("embedded KASANE Guard model must be valid")
    })
}

fn strategy_model() -> &'static TempoModel {
    static MODEL: OnceLock<TempoModel> = OnceLock::new();
    MODEL.get_or_init(|| {
        serde_json::from_str(STRATEGY_MODEL_JSON)
            .expect("embedded KASANE Strategy model must be valid")
    })
}

fn stack_ren_model() -> &'static StackRenModel {
    static MODEL: OnceLock<StackRenModel> = OnceLock::new();
    MODEL.get_or_init(|| {
        serde_json::from_str(STACK_REN_MODEL_JSON)
            .expect("embedded KASANE Stack-REN model must be valid")
    })
}

fn guard_policy() -> &'static GuardPolicy {
    static POLICY: OnceLock<GuardPolicy> = OnceLock::new();
    POLICY.get_or_init(|| {
        serde_json::from_str(GUARD_POLICY_JSON).expect("embedded KASANE Guard policy must be valid")
    })
}

fn strategy_policy() -> &'static StrategyPolicy {
    static POLICY: OnceLock<StrategyPolicy> = OnceLock::new();
    POLICY.get_or_init(|| {
        serde_json::from_str(STRATEGY_POLICY_JSON)
            .expect("embedded KASANE Strategy policy must be valid")
    })
}

fn stack_ren_policy() -> &'static StackRenPolicy {
    static POLICY: OnceLock<StackRenPolicy> = OnceLock::new();
    POLICY.get_or_init(|| {
        serde_json::from_str(STACK_REN_POLICY_JSON)
            .expect("embedded KASANE Stack-REN policy must be valid")
    })
}

fn piece_from_text(value: &str) -> Option<Piece> {
    match value.as_bytes().first().copied() {
        Some(b'I') => Some(Piece::I),
        Some(b'O') => Some(Piece::O),
        Some(b'T') => Some(Piece::T),
        Some(b'L') => Some(Piece::L),
        Some(b'J') => Some(Piece::J),
        Some(b'S') => Some(Piece::S),
        Some(b'Z') => Some(Piece::Z),
        _ => None,
    }
}

fn piece_to_byte(piece: Piece) -> u8 {
    match piece {
        Piece::I => b'I',
        Piece::O => b'O',
        Piece::T => b'T',
        Piece::L => b'L',
        Piece::J => b'J',
        Piece::S => b'S',
        Piece::Z => b'Z',
    }
}

fn make_board(player: &BrowserPlayer) -> Option<Board> {
    if player.board.len() != 400 {
        return None;
    }
    let current = piece_from_text(&player.current_piece)?;
    // Parse the real HOLD value even while HOLD is locked so malformed
    // browser snapshots remain rejected instead of being hidden by the lock.
    let hold_piece = match player.hold_piece.as_deref() {
        Some(piece) => Some(piece_from_text(piece)?),
        None => None,
    };
    let mut field = [[false; 10]; 40];
    for (source_y, row) in field.iter_mut().enumerate() {
        let simulator_y = 39 - source_y;
        for (x, cell) in row.iter_mut().enumerate() {
            *cell = player.board[simulator_y * 10 + x] != 0;
        }
    }

    let mut board = Board::new();
    board.set_field(field);
    board.b2b_bonus = player.is_b2_b;
    board.combo = (player.ren + 1).max(0) as u32;
    board.hold_piece = hold_piece;
    board.add_next_piece(current);
    for piece in &player.next_queue {
        board.add_next_piece(piece_from_text(piece)?);
    }
    Some(board)
}

fn make_phase(phase: &BrowserPhase) -> PhaseView {
    match phase.kind.as_str() {
        "lineClear" | "line_clear" => PhaseView::LineClear {
            ends_ms: phase.ends_ms,
        },
        "moving" => PhaseView::Moving {
            started_ms: phase.started_ms,
        },
        _ => PhaseView::Ready,
    }
}

fn make_view(player: &BrowserPlayer) -> Option<PlayerView> {
    Some(PlayerView {
        board: make_board(player)?,
        can_hold: player.can_hold,
        incoming: player
            .incoming
            .iter()
            .filter(|packet| packet.lines > 0)
            .map(|packet| IncomingPacket {
                lines: packet.lines,
                arrival_ms: packet.arrival_ms,
            })
            .collect(),
        phase: make_phase(&player.phase),
        pieces: player.pieces,
        average_piece_ms: player.average_piece_ms.max(50.0),
    })
}

fn rotation_number(rotation: RotationState) -> u8 {
    match rotation {
        RotationState::North => 0,
        RotationState::East => 1,
        RotationState::South => 2,
        RotationState::West => 3,
    }
}

fn simulator_anchor(piece: Piece, rotation: RotationState, x: i32, y: i32) -> (i32, i32) {
    let x_offset = match (piece, rotation) {
        (Piece::I, RotationState::North) | (Piece::I, RotationState::West) => -1,
        (Piece::I, RotationState::East) | (Piece::I, RotationState::South) => -2,
        _ => 0,
    };
    let y_offset = match (piece, rotation) {
        (Piece::I, RotationState::South) | (Piece::I, RotationState::West) => -1,
        _ => 0,
    };
    (x + x_offset, 39 - y + y_offset)
}

fn intent_number(intent: Intent) -> u8 {
    match intent {
        Intent::Stack => 0,
        Intent::SpikeNow => 1,
        Intent::CancellationDodge => 2,
        Intent::Counter => 3,
        Intent::TankThenFire => 4,
        Intent::Charge => 5,
        Intent::Dig => 6,
        Intent::ComboContinue => 7,
        Intent::Survival => 8,
    }
}

fn strategy_event_number(event: StrategyEvent) -> u8 {
    match event {
        StrategyEvent::None => 0,
        StrategyEvent::AttackExpert => 1,
        StrategyEvent::AttackExpertHoldFire => 2,
        StrategyEvent::SurvivalExpert => 3,
        StrategyEvent::SurvivalExpertHoldFire => 4,
        StrategyEvent::ChargeEnter => 5,
        StrategyEvent::ChargeContinue => 6,
        StrategyEvent::Armed => 7,
        StrategyEvent::ChargeReleaseIncomingEdge => 8,
        StrategyEvent::ChargeReleaseGarbageRiseEdge => 9,
        StrategyEvent::ReleaseDodge => 10,
        StrategyEvent::ReleaseCounter => 11,
        StrategyEvent::ReleaseImmediate => 12,
        StrategyEvent::ChargeAbort => 13,
        StrategyEvent::RenStart => 14,
        StrategyEvent::RenContinue => 15,
        StrategyEvent::StackRenEnter => 16,
        StrategyEvent::StackRenBuild => 17,
        StrategyEvent::StackRenFire => 18,
        StrategyEvent::StackRenContinue => 19,
        StrategyEvent::StackRenAbort => 20,
    }
}

fn policy_override_number(policy_override: PolicyOverride) -> u8 {
    match policy_override {
        PolicyOverride::None => 0,
        PolicyOverride::SamePlacementDodge => 1,
        PolicyOverride::SamePlacementCounter => 2,
        PolicyOverride::SamePlacementWait => 3,
        PolicyOverride::Placement => 4,
        PolicyOverride::PlacementAndWait => 5,
    }
}

fn configured_agent(snapshot: &BrowserSnapshot) -> (AgentKind, AgentConfig) {
    let is_guard = snapshot.model == "kasane-guard";
    let is_stack_ren = snapshot.model == "kasane-stack-ren";
    let is_strategy = snapshot.model == "kasane-strategy" || is_stack_ren;
    let mut config = AgentConfig {
        base_depth: 4,
        base_beam_width: 64,
        forecast_nodes: 300,
        maximum_wait_ms: 2_000,
        dodge_wait_cap_ms: 750,
        dodge_finish_height: 13,
        dodge_safety_margin: 5,
        base_guard_margin: 6.0,
        strict_base_policy: true,
        enable_tank: false,
        enable_charge: false,
        enable_tempo: snapshot.has_opponent && snapshot.model != "kasane-base",
        ..AgentConfig::default()
    };

    if is_guard {
        let policy = guard_policy();
        config.base_depth = policy.base_depth;
        config.base_beam_width = policy.base_beam_width;
        config.forecast_nodes = policy.forecast_nodes;
        config.maximum_wait_ms = policy.maximum_wait_ms;
        config.strict_base_policy = false;
        config.base_guard_margin = policy.base_guard_margin;
        config.guard_max_headroom_loss = policy.safety_gate.max_headroom_loss;
        config.guard_max_hole_increase = policy.safety_gate.max_hole_increase;
        config.guard_max_covered_increase = policy.safety_gate.max_covered_increase;
        config.guard_max_bumpiness_increase = policy.safety_gate.max_bumpiness_increase;
        config.guard_max_accessible_hole_loss = policy.safety_gate.max_accessible_hole_loss;
        config.guard_minimum_cancel_gain = policy.safety_gate.minimum_cancel_gain;
    }

    if is_strategy {
        let policy = strategy_policy();
        config.cold_clear_nodes = policy.cold_clear_floor_nodes.max(MIN_BROWSER_NODE_LIMIT);
        config.kasane_nodes = config.cold_clear_nodes;
        config.forecast_nodes = policy.forecast_nodes;
        config.maximum_wait_ms = policy.maximum_wait_ms;
        config.enable_tank = policy.enable_tank;
        config.strict_base_policy = false;
        config.strategy_min_advantage = policy.strategy_min_advantage;
        config.strategy_placement_min_advantage = policy.strategy_placement_min_advantage;
        config.strategy_max_alternatives = policy.strategy_max_alternatives;
        config.strategy_max_wait_ms = policy.strategy_max_wait_ms;
        config.strategy_enable_stateful = policy.strategy_enable_stateful;
        config.strategy_charge_min_pieces = policy.strategy_charge_min_pieces;
        config.strategy_charge_max_pieces = policy.strategy_charge_max_pieces;
        config.strategy_charge_min_headroom = policy.strategy_charge_min_headroom;
        config.strategy_release_window_ms = policy.strategy_release_window_ms;
        config.strategy_counter_offset_ms = policy.strategy_counter_offset_ms;
        config.strategy_enable_moe = policy.strategy_enable_moe;
        config.strategy_attack_enter_height = policy.strategy_attack_enter_height;
        config.strategy_attack_hard_finish_height = policy.strategy_attack_hard_finish_height;
        config.strategy_attack_stay_height = policy.strategy_attack_stay_height;
        config.strategy_attack_hole_burden = policy.strategy_attack_hole_burden;
        config.strategy_attack_min_headroom = policy.strategy_attack_min_headroom;
        config.strategy_attack_max_due_1000 = policy.strategy_attack_max_due_1000;
        config.strategy_enable_ren = policy.strategy_enable_ren;
        config.strategy_ren_depth = policy.strategy_ren_depth;
        config.strategy_ren_beam_width = policy.strategy_ren_beam_width;
        config.strategy_ren_start_chain = policy.strategy_ren_start_chain;
        config.strategy_ren_min_headroom = policy.strategy_ren_min_headroom;
    }

    if is_stack_ren {
        let policy = stack_ren_policy();
        config.stack_ren_enable = policy.stack_ren_enable;
        config.stack_ren_min_well_width = policy.stack_ren_min_well_width;
        config.stack_ren_max_well_width = policy.stack_ren_max_well_width;
        config.stack_ren_entry_interval = policy.stack_ren_entry_interval;
        config.stack_ren_action_limit = policy.stack_ren_action_limit;
        config.stack_ren_min_build_pieces = policy.stack_ren_min_build_pieces;
        config.stack_ren_max_build_pieces = policy.stack_ren_max_build_pieces;
        config.stack_ren_max_build_ms = policy.stack_ren_max_build_ms;
        config.stack_ren_target_depth = policy.stack_ren_target_depth;
        config.stack_ren_min_fire_chain = policy.stack_ren_min_fire_chain;
        config.stack_ren_min_fire_attack = policy.stack_ren_min_fire_attack;
        config.stack_ren_min_headroom = policy.stack_ren_min_headroom;
        config.stack_ren_max_holes = policy.stack_ren_max_holes;
        config.stack_ren_max_due_1000 = policy.stack_ren_max_due_1000;
        config.stack_ren_max_forecast_1000 = policy.stack_ren_max_forecast_1000;
        config.stack_ren_entry_threshold = policy.stack_ren_entry_threshold;
        config.stack_ren_fire_margin = policy.stack_ren_fire_margin;
        config.stack_ren_abort_margin = policy.stack_ren_abort_margin;
        config.stack_ren_hole_mismatch_penalty = policy.stack_ren_hole_mismatch_penalty;
        config.stack_ren_cooldown_pieces = policy.stack_ren_cooldown_pieces;
    }

    // Absence of the new fields is a strict compatibility mode. This is
    // important for existing workers and recorded snapshots: their search
    // choices must not drift merely because the wrapper was upgraded.
    if snapshot.node_limit.is_some() || snapshot.search_think_time_ms.is_some() {
        let time_tier = snapshot
            .search_think_time_ms
            .map(SearchTier::for_think_time);
        let bounded_nodes = snapshot
            .node_limit
            .map(|nodes| nodes.clamp(MIN_BROWSER_NODE_LIMIT, MAX_BROWSER_NODE_LIMIT));
        let node_tier = bounded_nodes.map(SearchTier::for_node_limit);
        let tier = match (time_tier, node_tier) {
            (Some(time), Some(nodes)) => time.min(nodes),
            (Some(time), None) => time,
            (None, Some(nodes)) => nodes,
            (None, None) => unreachable!("optional search budget was checked above"),
        };
        let profile = tier.profile(is_guard);
        config.base_depth = profile.base_depth;
        config.base_beam_width = profile.base_beam_width;
        config.forecast_nodes = bounded_nodes.map_or(profile.forecast_nodes, |nodes| {
            profile.forecast_nodes.min(nodes)
        });
        if is_strategy {
            // Strategy already pays for the equal-node persistent CC floor.
            // Keep its independent Base/forecast sidecars at the exact
            // depth4/beam64/forecast160 profile validated by the paired
            // bottom-12 benchmark; larger sidecars reduced PPS without
            // improving the selected policy.
            config.base_depth = config.base_depth.min(4);
            config.base_beam_width = config.base_beam_width.min(64);
            config.forecast_nodes = config.forecast_nodes.min(strategy_policy().forecast_nodes);
        }
        if is_stack_ren {
            config.strategy_ren_depth = profile.ren_depth;
            config.strategy_ren_beam_width = profile.ren_beam_width;
            config.stack_ren_action_limit = profile.stack_action_limit;
        }

        if let Some(nodes) = bounded_nodes {
            // Guard's embedded Cold Clear fallback now receives the same UI
            // node ceiling as standalone Cold Clear. `kasane_nodes` is kept
            // in sync for forward compatibility even though the current Base
            // beam search is governed by depth/width.
            config.cold_clear_nodes = nodes;
            config.kasane_nodes = nodes;
        }
    }

    let kind = if is_stack_ren {
        AgentKind::KasaneStackRen
    } else if is_guard {
        AgentKind::KasaneGuard
    } else {
        AgentKind::Kasane
    };
    (kind, config)
}

fn choose_with_cached_controller(
    model_id: &str,
    kind: AgentKind,
    config: AgentConfig,
    observation: &Observation,
) -> Option<SelectedAction> {
    CONTROLLER_CACHE.with(|slot| {
        let mut cache = slot.borrow_mut();
        if !cache.ensure(model_id, kind, config) {
            return None;
        }
        cache
            .current
            .as_mut()
            .and_then(|entry| entry.controller.choose(observation))
    })
}

fn reset_controller_cache() {
    CONTROLLER_CACHE.with(|slot| {
        *slot.borrow_mut() = ControllerCache::default();
    });
}

fn invalidate_controller_search() {
    CONTROLLER_CACHE.with(|slot| {
        if let Some(entry) = slot.borrow_mut().current.as_mut() {
            entry.controller.invalidate_search();
        }
    });
}

fn choose(snapshot: BrowserSnapshot) -> Option<KasaneMove> {
    let (kind, config) = configured_agent(&snapshot);
    let model_id = &snapshot.model;
    let rules = snapshot.rules.into_rules();
    let own = make_view(&snapshot.own)?;
    let opponent = make_view(&snapshot.opponent)?;
    let observation = Observation {
        now_ms: snapshot.now_ms,
        rules,
        own,
        opponent,
    };
    let selected = choose_with_cached_controller(model_id, kind, config, &observation)?;
    if !observation.own.can_hold && selected.action.hold {
        // Last-resort ABI guard. Root search is already HOLD-aware, so reaching
        // this branch indicates a future controller regression.
        invalidate_controller_search();
        return None;
    }
    debug_assert!(observation.own.can_hold || !selected.action.hold);
    let placement = selected.action.placement;
    let piece = placement.kind.0;
    let rotation = placement.kind.1;
    let (x, y) = simulator_anchor(piece, rotation, placement.x, placement.y);
    Some(KasaneMove {
        status: 1,
        piece: piece_to_byte(piece),
        hold: selected.action.hold as u8,
        rotation: rotation_number(rotation),
        tspin: match placement.tspin {
            TspinStatus::None => 0,
            TspinStatus::Mini => 1,
            TspinStatus::Full => 2,
        },
        x,
        y,
        wait_ms: selected.wait_ms.min(u32::MAX as u64) as u32,
        score: selected.score,
        intent: intent_number(selected.intent),
        reserved: [
            strategy_event_number(selected.strategy_event),
            policy_override_number(selected.policy_override),
            selected.strategy_detail,
        ],
        attack: kasane::search::attack_with_pc(
            &selected.action.lock,
            observation.rules.perfect_clear_special_attack,
        ),
    })
}

#[no_mangle]
pub unsafe extern "C" fn ks_choose_json(
    input: *const u8,
    input_len: u32,
    output: *mut KasaneMove,
) -> u32 {
    if input.is_null() || input_len == 0 || output.is_null() {
        return 0;
    }
    *output = KasaneMove::default();
    let bytes = slice::from_raw_parts(input, input_len as usize);
    let snapshot = match serde_json::from_slice::<BrowserSnapshot>(bytes) {
        Ok(snapshot) => snapshot,
        Err(_) => {
            (*output).status = 2;
            return 0;
        }
    };
    match choose(snapshot) {
        Some(result) => {
            *output = result;
            1
        }
        None => {
            (*output).status = 3;
            0
        }
    }
}

/// Explicit invalidation hook for worker lifecycle integration. Normal UI
/// budget/model changes do not require this call because the cache key detects
/// them, but reset/stop handlers may use it to release controller allocations.
#[no_mangle]
pub extern "C" fn ks_reset_controller_cache() {
    reset_controller_cache();
}

/// Drop only retained search DAGs after a garbage rise. The strategy memory
/// deliberately survives so the next observation can release an armed plan.
#[no_mangle]
pub extern "C" fn ks_invalidate_search() {
    invalidate_controller_search();
}

#[no_mangle]
pub unsafe extern "C" fn ks_alloc(size: usize) -> *mut u8 {
    if size == 0 {
        return std::ptr::null_mut();
    }
    let layout = std::alloc::Layout::from_size_align(size, 8).unwrap();
    std::alloc::alloc_zeroed(layout)
}

#[no_mangle]
pub unsafe extern "C" fn ks_dealloc(ptr: *mut u8, size: usize) {
    if ptr.is_null() || size == 0 {
        return;
    }
    let layout = std::alloc::Layout::from_size_align(size, 8).unwrap();
    std::alloc::dealloc(ptr, layout);
}

#[no_mangle]
pub extern "C" fn ks_move_size() -> usize {
    std::mem::size_of::<KasaneMove>()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::hint::black_box;
    use std::time::Instant;

    fn opening_player() -> BrowserPlayer {
        BrowserPlayer {
            board: vec![0; 400],
            current_piece: "I".to_owned(),
            next_queue: ["O", "T", "S", "Z", "L", "J", "I", "O"]
                .into_iter()
                .map(str::to_owned)
                .collect(),
            hold_piece: None,
            can_hold: true,
            is_b2_b: false,
            ren: -1,
            incoming: Vec::new(),
            phase: BrowserPhase {
                kind: "moving".to_owned(),
                started_ms: 0,
                ends_ms: 0,
            },
            pieces: 0,
            average_piece_ms: 350.0,
        }
    }

    fn opening_snapshot(model: &str) -> BrowserSnapshot {
        let player = opening_player();
        BrowserSnapshot {
            now_ms: 0,
            own: player.clone(),
            opponent: player,
            has_opponent: true,
            model: model.to_owned(),
            rules: BrowserRules::default(),
            search_think_time_ms: None,
            node_limit: None,
        }
    }

    fn cache_metrics() -> (u32, u32) {
        CONTROLLER_CACHE.with(|slot| {
            let cache = slot.borrow();
            (cache.builds, cache.hits)
        })
    }

    #[test]
    fn move_abi_is_stable() {
        assert_eq!(std::mem::size_of::<KasaneMove>(), 32);
    }

    #[test]
    fn embedded_models_parse() {
        assert!(base_model().feature_names.len() > 100);
        assert_eq!(tempo_model().feature_names.len(), 44);
        assert_eq!(guard_model().feature_names.len(), 44);
        assert_eq!(strategy_model().feature_names.len(), 116);
        assert_eq!(stack_ren_model().feature_names.len(), 56);
        assert_eq!(guard_policy().maximum_wait_ms, 0);
        assert!(strategy_policy().strategy_enable_stateful);
        assert_eq!(strategy_policy().strategy_release_window_ms, 750);
        assert!(strategy_policy().strategy_enable_moe);
        assert_eq!(strategy_policy().strategy_attack_hard_finish_height, 11);
        assert_eq!(strategy_policy().strategy_attack_min_headroom, 8);
        assert!(stack_ren_policy().stack_ren_enable);
        assert!((2..=4).contains(&stack_ren_policy().stack_ren_min_well_width));
        assert!((2..=4).contains(&stack_ren_policy().stack_ren_max_well_width));
    }

    #[test]
    fn browser_rules_follow_live_settings_instead_of_training_pins() {
        let rules = BrowserRules {
            input_interval_ms: 37,
            line_clear_delay_ms: 250,
            garbage_grace_ms: 1_300,
            decision_latency_ms: 90,
            preview_count: 6,
            garbage_randomness: 0.42,
            perfect_clear_special_attack: 10,
            spawn_delay_ms: 75,
        }
        .into_rules();
        assert_eq!(rules.input_interval_ms, 37);
        assert_eq!(rules.line_clear_delay_ms, 250);
        assert_eq!(rules.garbage_grace_ms, 1_300);
        assert_eq!(rules.decision_latency_ms, 90);
        assert_eq!(rules.preview_count, 6);
        assert_eq!(rules.perfect_clear_special_attack, 10);
        assert_eq!(rules.spawn_delay_ms, 75);
    }

    #[test]
    fn omitted_search_budget_preserves_v1_settings() {
        let basic = opening_snapshot("kasane-basic");
        let (_, basic_config) = configured_agent(&basic);
        assert_eq!(basic_config.cold_clear_nodes, 1_500);
        assert_eq!(basic_config.base_depth, 4);
        assert_eq!(basic_config.base_beam_width, 64);
        assert_eq!(basic_config.forecast_nodes, 300);

        let guard = opening_snapshot("kasane-guard");
        let (_, guard_config) = configured_agent(&guard);
        assert_eq!(guard_config.cold_clear_nodes, 1_500);
        assert_eq!(guard_config.base_depth, 2);
        assert_eq!(guard_config.base_beam_width, 12);
        assert_eq!(guard_config.forecast_nodes, 40);
    }

    #[test]
    fn live_ui_budget_reaches_guard_fallback_and_enhanced_profile() {
        let mut snapshot = opening_snapshot("kasane-guard");
        snapshot.rules.decision_latency_ms = 50;
        snapshot.node_limit = Some(120_000);
        let (_, config) = configured_agent(&snapshot);
        assert_eq!(config.cold_clear_nodes, 120_000);
        assert_eq!(config.kasane_nodes, 120_000);
        assert_eq!(config.base_depth, 3);
        assert_eq!(config.base_beam_width, 48);
        assert_eq!(config.forecast_nodes, 320);
    }

    #[test]
    fn strategy_ui_budget_keeps_the_validated_sidecar_profile() {
        let mut snapshot = opening_snapshot("kasane-strategy");
        snapshot.search_think_time_ms = Some(180);
        snapshot.node_limit = Some(120_000);
        let (_, config) = configured_agent(&snapshot);
        assert_eq!(config.cold_clear_nodes, 120_000);
        assert_eq!(config.kasane_nodes, 120_000);
        assert_eq!(config.base_depth, 4);
        assert_eq!(config.base_beam_width, 64);
        assert_eq!(config.forecast_nodes, 160);
    }

    #[test]
    fn stack_ren_budget_scales_its_own_proof_search() {
        let mut economy = opening_snapshot("kasane-stack-ren");
        economy.search_think_time_ms = Some(8);
        economy.node_limit = Some(5_000);
        let (_, economy_config) = configured_agent(&economy);
        assert_eq!(economy_config.strategy_ren_depth, 4);
        assert_eq!(economy_config.strategy_ren_beam_width, 10);
        assert_eq!(economy_config.stack_ren_action_limit, 24);

        let mut production = opening_snapshot("kasane-stack-ren");
        production.search_think_time_ms = Some(180);
        production.node_limit = Some(120_000);
        let (_, production_config) = configured_agent(&production);
        assert_eq!(production_config.strategy_ren_depth, 6);
        assert_eq!(production_config.strategy_ren_beam_width, 20);
        assert_eq!(production_config.stack_ren_action_limit, 48);

        let mut maximum = production;
        maximum.node_limit = Some(200_000);
        let (_, maximum_config) = configured_agent(&maximum);
        assert_eq!(maximum_config.strategy_ren_depth, 7);
        assert_eq!(maximum_config.strategy_ren_beam_width, 28);
        assert_eq!(maximum_config.stack_ren_action_limit, 64);
    }

    #[test]
    fn explicit_think_budget_and_node_limit_use_the_tighter_tier() {
        let mut time_limited = opening_snapshot("kasane-basic");
        time_limited.search_think_time_ms = Some(8);
        time_limited.node_limit = Some(200_000);
        let (_, time_config) = configured_agent(&time_limited);
        assert_eq!(time_config.base_depth, 3);
        assert_eq!(time_config.base_beam_width, 24);
        assert_eq!(time_config.forecast_nodes, 96);

        let mut node_limited = opening_snapshot("kasane-basic");
        node_limited.search_think_time_ms = Some(200);
        node_limited.node_limit = Some(5_000);
        let (_, node_config) = configured_agent(&node_limited);
        assert_eq!(node_config.base_depth, 3);
        assert_eq!(node_config.base_beam_width, 24);
        assert_eq!(node_config.forecast_nodes, 96);
    }

    #[test]
    fn tactical_decision_latency_does_not_control_search_budget() {
        let mut low_latency = opening_snapshot("kasane-basic");
        low_latency.rules.decision_latency_ms = 1;
        low_latency.node_limit = Some(200_000);
        let (_, low_latency_config) = configured_agent(&low_latency);

        let mut high_latency = low_latency.clone();
        high_latency.rules.decision_latency_ms = 2_000;
        let (_, high_latency_config) = configured_agent(&high_latency);
        assert_eq!(low_latency_config.base_depth, 5);
        assert_eq!(low_latency_config.base_beam_width, 112);
        assert_eq!(
            ControllerKey::new(AgentKind::Kasane, &low_latency_config),
            ControllerKey::new(AgentKind::Kasane, &high_latency_config)
        );
    }

    #[test]
    fn node_limit_is_safely_clamped() {
        let mut too_large = opening_snapshot("kasane-guard");
        too_large.search_think_time_ms = Some(200);
        too_large.node_limit = Some(u32::MAX);
        let (_, large_config) = configured_agent(&too_large);
        assert_eq!(large_config.cold_clear_nodes, MAX_BROWSER_NODE_LIMIT);

        let mut too_small = opening_snapshot("kasane-guard");
        too_small.search_think_time_ms = Some(0);
        too_small.node_limit = Some(0);
        let (_, small_config) = configured_agent(&too_small);
        assert_eq!(small_config.cold_clear_nodes, MIN_BROWSER_NODE_LIMIT);
        assert_eq!(small_config.forecast_nodes, MIN_BROWSER_NODE_LIMIT);
    }

    #[test]
    fn controller_cache_reuses_and_rebuilds_on_effective_setting_change() {
        reset_controller_cache();
        let mut snapshot = opening_snapshot("kasane-guard");
        snapshot.search_think_time_ms = Some(8);
        snapshot.node_limit = Some(5_000);
        let (kind, config) = configured_agent(&snapshot);
        CONTROLLER_CACHE.with(|slot| {
            let mut cache = slot.borrow_mut();
            assert!(cache.ensure(&snapshot.model, kind, config.clone()));
            let first_address = cache
                .current
                .as_ref()
                .map(|entry| std::ptr::from_ref(&entry.controller).cast::<()>() as usize);
            assert!(cache.ensure(&snapshot.model, kind, config));
            let second_address = cache
                .current
                .as_ref()
                .map(|entry| std::ptr::from_ref(&entry.controller).cast::<()>() as usize);
            assert_eq!(first_address, second_address);
        });
        assert_eq!(cache_metrics(), (1, 1));

        snapshot.node_limit = Some(6_000);
        let (kind, config) = configured_agent(&snapshot);
        CONTROLLER_CACHE.with(|slot| {
            assert!(slot.borrow_mut().ensure(&snapshot.model, kind, config));
        });
        assert_eq!(cache_metrics(), (2, 1));
    }

    #[test]
    fn model_id_is_an_independent_controller_cache_key() {
        reset_controller_cache();
        let basic = opening_snapshot("kasane-basic");
        let alias = opening_snapshot("future-strategy-id");
        let (basic_kind, basic_config) = configured_agent(&basic);
        let (alias_kind, alias_config) = configured_agent(&alias);
        assert_eq!(
            ControllerKey::new(basic_kind, &basic_config),
            ControllerKey::new(alias_kind, &alias_config)
        );

        CONTROLLER_CACHE.with(|slot| {
            let mut cache = slot.borrow_mut();
            assert!(cache.ensure(&basic.model, basic_kind, basic_config));
            assert!(cache.ensure(&alias.model, alias_kind, alias_config));
        });
        assert_eq!(cache_metrics(), (2, 0));
    }

    #[test]
    fn explicit_reset_drops_the_persistent_controller() {
        reset_controller_cache();
        let snapshot = opening_snapshot("kasane-basic");
        let (kind, config) = configured_agent(&snapshot);
        CONTROLLER_CACHE.with(|slot| {
            assert!(slot.borrow_mut().ensure(&snapshot.model, kind, config));
            assert!(slot.borrow().current.is_some());
        });
        assert_eq!(cache_metrics(), (1, 0));

        ks_reset_controller_cache();
        assert_eq!(cache_metrics(), (0, 0));
        CONTROLLER_CACHE.with(|slot| assert!(slot.borrow().current.is_none()));
    }

    #[test]
    fn camel_case_budget_fields_extend_the_json_abi() {
        let player = serde_json::json!({
            "board": vec![0; 400],
            "currentPiece": "I",
            "nextQueue": ["O", "T", "S", "Z", "L", "J", "I", "O"],
            "phase": { "kind": "moving", "startedMs": 0 },
            "averagePieceMs": 350.0
        });
        let payload = serde_json::json!({
            "nowMs": 0,
            "own": player.clone(),
            "opponent": player,
            "hasOpponent": true,
            "model": "kasane-guard",
            "rules": { "decisionLatencyMs": 50 },
            "searchThinkTimeMs": 50,
            "nodeLimit": 120000
        });
        let parsed: BrowserSnapshot = serde_json::from_value(payload.clone()).unwrap();
        assert!(
            parsed.own.can_hold,
            "legacy payloads must default canHold to true"
        );
        assert_eq!(parsed.search_think_time_ms, Some(50));
        assert_eq!(parsed.node_limit, Some(120_000));

        let mut locked_hold = payload.clone();
        locked_hold["own"]["canHold"] = serde_json::json!(false);
        let parsed_locked: BrowserSnapshot = serde_json::from_value(locked_hold).unwrap();
        assert!(!parsed_locked.own.can_hold);

        let mut legacy_alias = payload;
        let object = legacy_alias.as_object_mut().unwrap();
        object.remove("searchThinkTimeMs");
        object.insert("thinkTimeMs".to_owned(), serde_json::json!(35));
        let parsed_alias: BrowserSnapshot = serde_json::from_value(legacy_alias).unwrap();
        assert_eq!(parsed_alias.search_think_time_ms, Some(35));
    }

    #[test]
    fn stack_ren_never_generates_or_returns_hold_when_can_hold_is_false() {
        reset_controller_cache();
        let mut snapshot = opening_snapshot("kasane-stack-ren");
        snapshot.own.can_hold = false;
        snapshot.own.hold_piece = Some("T".to_owned());

        let board = make_board(&snapshot.own).expect("locked-HOLD snapshot should be valid");
        let actions = kasane::search::legal_actions_with_hold(&board, snapshot.own.can_hold);
        assert!(!actions.is_empty());
        assert!(
            actions.iter().all(|action| !action.hold),
            "canHold=false must remove every HOLD action at the search root"
        );

        let result = choose(snapshot).expect("Stack-REN should retain a non-HOLD placement");
        assert_eq!(result.status, 1);
        assert_eq!(
            result.hold, 0,
            "WASM must never return HOLD while it is locked"
        );
    }

    #[test]
    fn locked_hold_does_not_mask_unknown_piece_rejection() {
        let mut player = opening_player();
        player.can_hold = false;
        player.hold_piece = Some("?".to_owned());
        assert!(make_board(&player).is_none());

        let mut player = opening_player();
        player.can_hold = false;
        player.next_queue[0] = "?".to_owned();
        assert!(make_board(&player).is_none());
    }

    #[test]
    #[ignore = "microbenchmark; run with --release --ignored --nocapture"]
    fn controller_construction_cache_benchmark() {
        let mut snapshot = opening_snapshot("kasane-guard");
        snapshot.search_think_time_ms = Some(50);
        snapshot.node_limit = Some(120_000);
        let (kind, config) = configured_agent(&snapshot);
        let iterations = 10_000_u32;

        let cold_started = Instant::now();
        let mut cold_builds = 0_u32;
        for _ in 0..iterations {
            let mut cache = ControllerCache::default();
            black_box(cache.ensure(&snapshot.model, kind, config.clone()));
            cold_builds = cold_builds.saturating_add(cache.builds);
        }
        let cold_elapsed = cold_started.elapsed();

        let warm_started = Instant::now();
        let mut cache = ControllerCache::default();
        for _ in 0..iterations {
            black_box(cache.ensure(&snapshot.model, kind, config.clone()));
        }
        let warm_elapsed = warm_started.elapsed();
        println!(
            "controller setup x{iterations}: cold={cold_elapsed:?} ({cold_builds} builds), cached={warm_elapsed:?} ({} build, {} hits)",
            cache.builds, cache.hits
        );
        assert_eq!(cold_builds, iterations);
        assert_eq!(cache.builds, 1);
        assert_eq!(cache.hits, iterations - 1);
    }

    #[test]
    #[ignore = "search benchmark; run with --release --ignored --nocapture"]
    fn ui_budget_opening_inference_benchmark() {
        let legacy = opening_snapshot("kasane-guard");
        reset_controller_cache();
        let legacy_started = Instant::now();
        let legacy_move = black_box(choose(legacy));
        let legacy_elapsed = legacy_started.elapsed();

        let mut budgeted = opening_snapshot("kasane-guard");
        budgeted.rules.decision_latency_ms = 50;
        budgeted.search_think_time_ms = Some(50);
        budgeted.node_limit = Some(120_000);
        reset_controller_cache();
        let budgeted_started = Instant::now();
        let budgeted_move = black_box(choose(budgeted));
        let budgeted_elapsed = budgeted_started.elapsed();

        println!(
            "Guard opening inference: legacy-1500={legacy_elapsed:?}, ui-120000={budgeted_elapsed:?}"
        );
        assert!(legacy_move.is_some());
        assert!(budgeted_move.is_some());
    }

    #[test]
    fn basic_model_returns_a_reachable_opening_move() {
        let result = choose(opening_snapshot("kasane-basic"))
            .expect("KASANE should find an opening placement");
        assert_eq!(result.status, 1);
        assert!(b"IOTLSJZ".contains(&result.piece));
        assert!(result.x >= -2 && result.x <= 9);
        assert!(result.y >= 0 && result.y < 40);
    }

    #[test]
    fn guard_model_returns_a_reachable_opening_move() {
        let result = choose(opening_snapshot("kasane-guard"))
            .expect("KASANE Guard should find an opening placement");
        assert_eq!(result.status, 1);
        assert!(b"IOTLSJZ".contains(&result.piece));
        assert!(result.x >= -2 && result.x <= 9);
        assert!(result.y >= 0 && result.y < 40);
    }

    #[test]
    fn strategy_model_returns_a_reachable_opening_move() {
        let result = choose(opening_snapshot("kasane-strategy"))
            .expect("KASANE Strategy should retain the Cold Clear floor");
        assert_eq!(result.status, 1);
        assert!(b"IOTLSJZ".contains(&result.piece));
        assert!(result.x >= -2 && result.x <= 9);
        assert!(result.y >= 0 && result.y < 40);
    }

    #[test]
    fn stack_ren_model_is_a_distinct_reachable_controller() {
        let snapshot = opening_snapshot("kasane-stack-ren");
        let (kind, config) = configured_agent(&snapshot);
        assert_eq!(kind, AgentKind::KasaneStackRen);
        assert!(config.stack_ren_enable);
        assert_eq!(config.stack_ren_min_well_width, 2);
        assert_eq!(config.stack_ren_max_well_width, 4);
        assert_eq!(config.stack_ren_min_headroom, 12);
        assert_eq!(config.stack_ren_max_due_1000, 0);
        assert_eq!(config.stack_ren_entry_threshold, 96.0);
        let result = choose(snapshot).expect("Stack-REN should find an opening placement");
        assert_eq!(result.status, 1);
        assert!(b"IOTLSJZ".contains(&result.piece));
        assert!(result.x >= -2 && result.x <= 9);
        assert!(result.y >= 0 && result.y < 40);
        assert_ne!(
            result.reserved[0],
            strategy_event_number(StrategyEvent::StackRenEnter),
            "the validated finisher gate must not spend tempo on an empty-board opening"
        );
    }
}
