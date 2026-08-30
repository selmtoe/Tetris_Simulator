//! Raw browser ABI for the KASANE Base + Tempo controller.
//!
//! The simulator sends one complete two-player observation as JSON.  This
//! wrapper only converts coordinate systems and owns the embedded, versioned
//! model artifacts; all placement and tactical decisions remain in `kasane`.

#![allow(clippy::missing_safety_doc)]

use kasane::agent::{
    AgentConfig, AgentController, AgentKind, IncomingPacket, Intent, Observation, PhaseView,
    PlayerView,
};
use kasane::base::BaseModel;
use kasane::model::TempoModel;
use kasane::Rules;
use libtetris::{Board, Piece, RotationState, TspinStatus};
use serde::Deserialize;
use std::slice;
use std::sync::OnceLock;

const BASE_MODEL_JSON: &str = include_str!("../../../kasane/models/base-model-evolved-v3.json");
const TEMPO_MODEL_JSON: &str = include_str!("../../../kasane/config/tempo-model-bootstrap-v3.json");
const GUARD_MODEL_JSON: &str = include_str!("../../../kasane/models/guard-model-evolved-v1.json");
const GUARD_POLICY_JSON: &str = include_str!("../../../kasane/config/kasane-guard-v1.json");

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
}

fn default_ren() -> i32 {
    -1
}

fn default_piece_ms() -> f32 {
    350.0
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

fn guard_policy() -> &'static GuardPolicy {
    static POLICY: OnceLock<GuardPolicy> = OnceLock::new();
    POLICY.get_or_init(|| {
        serde_json::from_str(GUARD_POLICY_JSON).expect("embedded KASANE Guard policy must be valid")
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
    let mut field = [[false; 10]; 40];
    for source_y in 0..40 {
        let simulator_y = 39 - source_y;
        for x in 0..10 {
            field[source_y][x] = player.board[simulator_y * 10 + x] != 0;
        }
    }

    let mut board = Board::new();
    board.set_field(field);
    board.b2b_bonus = player.is_b2_b;
    board.combo = (player.ren + 1).max(0) as u32;
    board.hold_piece = player.hold_piece.as_deref().and_then(piece_from_text);
    board.add_next_piece(current);
    for piece in &player.next_queue {
        if let Some(piece) = piece_from_text(piece) {
            board.add_next_piece(piece);
        }
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

fn choose(snapshot: BrowserSnapshot) -> Option<KasaneMove> {
    let rules = snapshot.rules.into_rules();
    let own = make_view(&snapshot.own)?;
    let opponent = make_view(&snapshot.opponent)?;
    let observation = Observation {
        now_ms: snapshot.now_ms,
        rules,
        own,
        opponent,
    };

    let is_guard = snapshot.model == "kasane-guard";
    let mut config = AgentConfig::default();
    config.base_depth = 4;
    config.base_beam_width = 64;
    config.forecast_nodes = 300;
    config.maximum_wait_ms = 2_000;
    config.dodge_wait_cap_ms = 750;
    config.dodge_finish_height = 13;
    config.dodge_safety_margin = 5;
    config.base_guard_margin = 6.0;
    config.strict_base_policy = true;
    config.enable_tank = false;
    config.enable_charge = false;
    config.enable_tempo = snapshot.has_opponent && snapshot.model != "kasane-base";
    config.base_model_override = Some(base_model().clone());
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
        config.tempo_model_override = Some(guard_model().clone());
    } else {
        config.tempo_model_override = Some(tempo_model().clone());
    }

    let kind = if is_guard {
        AgentKind::KasaneGuard
    } else {
        AgentKind::Kasane
    };
    let mut controller = AgentController::new(kind, config).ok()?;
    let selected = controller.choose(&observation)?;
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
        reserved: [0; 3],
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

    fn opening_player() -> BrowserPlayer {
        BrowserPlayer {
            board: vec![0; 400],
            current_piece: "I".to_owned(),
            next_queue: ["O", "T", "S", "Z", "L", "J", "I", "O"]
                .into_iter()
                .map(str::to_owned)
                .collect(),
            hold_piece: None,
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

    #[test]
    fn move_abi_is_stable() {
        assert_eq!(std::mem::size_of::<KasaneMove>(), 32);
    }

    #[test]
    fn embedded_models_parse() {
        assert!(base_model().feature_names.len() > 100);
        assert_eq!(tempo_model().feature_names.len(), 44);
        assert_eq!(guard_model().feature_names.len(), 44);
        assert_eq!(guard_policy().maximum_wait_ms, 0);
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
    fn basic_model_returns_a_reachable_opening_move() {
        let player = opening_player();
        let result = choose(BrowserSnapshot {
            now_ms: 0,
            own: player.clone(),
            opponent: player,
            has_opponent: true,
            model: "kasane-basic".to_owned(),
            rules: BrowserRules::default(),
        })
        .expect("KASANE should find an opening placement");
        assert_eq!(result.status, 1);
        assert!(b"IOTLSJZ".contains(&result.piece));
        assert!(result.x >= -2 && result.x <= 9);
        assert!(result.y >= 0 && result.y < 40);
    }

    #[test]
    fn guard_model_returns_a_reachable_opening_move() {
        let player = opening_player();
        let result = choose(BrowserSnapshot {
            now_ms: 0,
            own: player.clone(),
            opponent: player,
            has_opponent: true,
            model: "kasane-guard".to_owned(),
            rules: BrowserRules::default(),
        })
        .expect("KASANE Guard should find an opening placement");
        assert_eq!(result.status, 1);
        assert!(b"IOTLSJZ".contains(&result.piece));
        assert!(result.x >= -2 && result.x <= 9);
        assert!(result.y >= 0 && result.y < 40);
    }
}
