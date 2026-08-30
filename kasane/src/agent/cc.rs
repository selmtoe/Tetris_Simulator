use super::{AgentConfig, IncomingPacket, Observation, PhaseView, SelectedAction};
use crate::search::{attack_with_pc, legal_actions, same_action, ActionKey, PlacementAction};
use libtetris::{Board, MovementMode, SpawnRule};
#[cfg(not(target_arch = "wasm32"))]
use simulator_cold_clear_wasm::seed_deterministic_search;
use simulator_cold_clear_wasm::{evaluation::Standard, BotState, Options};
use std::collections::HashMap;
use std::hash::{Hash, Hasher};

#[derive(Clone, Debug)]
pub struct CcAnalysis {
    pub chosen: Option<PlacementAction>,
    pub root_scores: HashMap<ActionKey, (i32, i32)>,
    pub nodes: u32,
}

#[derive(Clone, Debug, Default)]
pub struct OpponentForecast {
    pub action: Option<PlacementAction>,
    pub lock_ms: u64,
    pub raw_attack: u32,
    pub outgoing_attack: u32,
    pub confidence: f32,
    pub events: Vec<ForecastEvent>,
}

#[derive(Clone, Debug)]
pub struct ForecastEvent {
    pub action: PlacementAction,
    pub lock_ms: u64,
    pub raw_attack: u32,
    pub outgoing_attack: u32,
}

pub fn analyze_cold_clear(board: &Board, nodes: u32, incoming: u32) -> CcAnalysis {
    analyze_cold_clear_with_pc(board, nodes, incoming, 0)
}

fn analyze_cold_clear_with_pc(
    board: &Board,
    nodes: u32,
    incoming: u32,
    perfect_clear_special_attack: u32,
) -> CcAnalysis {
    let actions = legal_actions(board);
    if actions.is_empty() {
        return CcAnalysis {
            chosen: None,
            root_scores: HashMap::new(),
            nodes: 0,
        };
    }

    let target = nodes.max(32);
    seed_search(board, target, incoming);
    let mut evaluator = Standard::default();
    // Offline PC0 experiments remove the bonus. Live simulator matches keep
    // Cold Clear Standard's normal perfect-clear preference.
    if perfect_clear_special_attack == 0 {
        evaluator.perfect_clear = 0;
    }
    evaluator.stack_pc_damage = true;
    let options = Options {
        mode: MovementMode::ZeroG,
        spawn_rule: SpawnRule::Row19Or20,
        use_hold: true,
        speculate: true,
        min_nodes: 0,
        max_nodes: target.saturating_mul(2),
        threads: 1,
    };
    let mut state = BotState::new(board.clone(), options);
    let mut stalled = 0;
    while state.node_count() < target || !state.min_thinking_reached() {
        match state.think() {
            Ok(thinker) => {
                let result = thinker.think(&evaluator);
                state.finish_thinking(result);
                stalled = 0;
            }
            Err(_) => {
                stalled += 1;
                if stalled >= 2 {
                    break;
                }
            }
        }
    }

    let mut root_scores = HashMap::new();
    for candidate in state.candidate_scores() {
        if let Some(action) = actions
            .iter()
            .find(|action| same_action(action, candidate.mv, candidate.hold))
        {
            root_scores.insert(action.key(), (candidate.value, candidate.spike));
        }
    }
    let chosen = state
        .suggest_move(&evaluator, None, incoming)
        .and_then(|(mv, _)| {
            actions
                .iter()
                .find(|action| same_action(action, mv.expected_location, mv.hold))
                .cloned()
        })
        .or_else(|| safest_fallback(&actions));

    CcAnalysis {
        chosen,
        root_scores,
        nodes: state.node_count(),
    }
}

#[cfg(not(target_arch = "wasm32"))]
fn seed_search(board: &Board, nodes: u32, incoming: u32) {
    seed_deterministic_search(search_seed(board, nodes, incoming));
}

// The browser build of the bundled Cold Clear DAG already uses its
// deterministic single-threaded WASM RNG.  There is no process-global seed
// hook on wasm32, so the native benchmark seeding call intentionally becomes
// a no-op here.
#[cfg(target_arch = "wasm32")]
fn seed_search(_board: &Board, _nodes: u32, _incoming: u32) {}

fn search_seed(board: &Board, nodes: u32, incoming: u32) -> u64 {
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    board.get_field().hash(&mut hasher);
    board.combo.hash(&mut hasher);
    board.b2b_bonus.hash(&mut hasher);
    board.hold_piece.hash(&mut hasher);
    for piece in board.next_queue() {
        piece.hash(&mut hasher);
    }
    nodes.hash(&mut hasher);
    incoming.hash(&mut hasher);
    hasher.finish()
}

pub fn choose_cold_clear(
    observation: &Observation,
    config: &AgentConfig,
) -> Option<SelectedAction> {
    let analysis = analyze_cold_clear_with_pc(
        &observation.own.board,
        config.cold_clear_nodes,
        observation.own.incoming_total(),
        observation.rules.perfect_clear_special_attack,
    );
    let action = analysis.chosen?;
    let (value, spike) = analysis
        .root_scores
        .get(&action.key())
        .copied()
        .unwrap_or((0, 0));
    Some(SelectedAction {
        action,
        wait_ms: 0,
        intent: super::Intent::Stack,
        score: value as f32 + spike as f32 * 100.0,
    })
}

pub fn forecast_opponent(observation: &Observation, config: &AgentConfig) -> OpponentForecast {
    let first_start_ms = match observation.opponent.phase {
        PhaseView::Ready => observation.now_ms,
        PhaseView::Moving { started_ms } => started_ms,
        PhaseView::LineClear { ends_ms } => ends_ms,
    };
    let mut board = observation.opponent.board.clone();
    let mut pending = observation.opponent.incoming_total();
    let mut next_start_ms = first_start_ms;
    let mut confidence_sum = 0.0;
    let mut events = Vec::with_capacity(4);
    for step in 0..4 {
        let nodes = if step == 0 {
            config.forecast_nodes
        } else {
            (config.forecast_nodes * 2 / 3).max(64)
        };
        let analysis = analyze_cold_clear_with_pc(
            &board,
            nodes,
            pending,
            observation.rules.perfect_clear_special_attack,
        );
        let Some(action) = analysis.chosen else {
            break;
        };
        let duration = observation.rules.decision_latency_ms
            + observation
                .rules
                .controller_time_ms(action.movements.len(), action.hold);
        let predicted = next_start_ms.saturating_add(duration);
        let lock_ms = if step == 0 && predicted <= observation.now_ms {
            // An observed active piece may be slower than its reconstructed
            // Cold Clear path. Blend in measured pace for the remaining time.
            let measured = observation.opponent.average_piece_ms.max(50.0) as u64;
            observation
                .now_ms
                .saturating_add((measured / 4).max(observation.rules.input_interval_ms))
        } else {
            predicted
        };
        let raw_attack =
            attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
        let outgoing_attack = raw_attack.saturating_sub(pending);
        pending = pending.saturating_sub(raw_attack);
        confidence_sum += (analysis.nodes as f32 / nodes.max(1) as f32).min(1.0);
        next_start_ms = lock_ms
            + if action.lock.cleared_lines.is_empty() {
                0
            } else {
                observation.rules.line_clear_delay_ms
            };
        board = action.board_after.clone();
        events.push(ForecastEvent {
            action,
            lock_ms,
            raw_attack,
            outgoing_attack,
        });
    }
    let Some(first) = events.first() else {
        return OpponentForecast::default();
    };
    OpponentForecast {
        action: Some(first.action.clone()),
        lock_ms: first.lock_ms,
        raw_attack: first.raw_attack,
        outgoing_attack: first.outgoing_attack,
        confidence: confidence_sum / events.len() as f32,
        events,
    }
}

fn safest_fallback(actions: &[PlacementAction]) -> Option<PlacementAction> {
    actions
        .iter()
        .min_by_key(|action| {
            (
                action.lock.locked_out,
                *action
                    .board_after
                    .column_heights()
                    .iter()
                    .max()
                    .unwrap_or(&40),
                action.movements.len(),
            )
        })
        .cloned()
}

#[allow(dead_code)]
fn _incoming_with_prediction(
    current: &[IncomingPacket],
    lines: u32,
    arrival_ms: u64,
) -> Vec<IncomingPacket> {
    let mut packets = current.to_vec();
    if lines > 0 {
        packets.push(IncomingPacket { lines, arrival_ms });
    }
    packets
}

#[cfg(test)]
mod tests {
    use super::*;
    use libtetris::Piece;

    #[test]
    fn root_scores_cover_the_reachable_cold_clear_frontier() {
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
        let analysis = analyze_cold_clear(&board, 300, 0);
        let reachable = legal_actions(&board).len();
        assert!(analysis.chosen.is_some());
        assert!(analysis.root_scores.len() >= reachable / 2);
        assert!(analysis
            .chosen
            .as_ref()
            .and_then(|action| analysis.root_scores.get(&action.key()))
            .is_some());
    }

    #[test]
    fn deterministic_search_repeats_exactly() {
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
        let first = analyze_cold_clear(&board, 700, 0);
        let second = analyze_cold_clear(&board, 700, 0);
        assert_eq!(
            first.chosen.as_ref().map(PlacementAction::key),
            second.chosen.as_ref().map(PlacementAction::key)
        );
        assert_eq!(first.root_scores, second.root_scores);
        assert_eq!(first.nodes, second.nodes);
    }
}
