use super::{matured_lines, AgentConfig, IncomingPacket, Observation, PhaseView, SelectedAction};
#[cfg(test)]
use crate::search::legal_actions;
use crate::search::{
    attack_with_pc, legal_actions_with_hold, same_action, ActionKey, PlacementAction,
};
use libtetris::{Board, MovementMode, SpawnRule};
use simulator_cold_clear_wasm::seed_deterministic_search;
use simulator_cold_clear_wasm::{evaluation::Standard, BotState, Options};
use std::collections::HashMap;

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

impl ForecastEvent {
    /// The first event's cancellation is reconstructed against the exact
    /// current queue. For later events the lightweight board forecast cannot
    /// fully replay garbage rises, so raw attack is the safe threat bound.
    pub(crate) fn conservative_threat(&self, event_index: usize) -> u32 {
        if event_index == 0 {
            self.outgoing_attack
        } else {
            self.raw_attack
        }
    }
}

struct PersistentColdClear {
    state: BotState<Standard>,
    evaluator: Standard,
    mirror: Board,
    target_nodes: u32,
    perfect_clear_special_attack: u32,
    commit_candidates: HashMap<ActionKey, libtetris::FallingPiece>,
}

#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
pub(crate) struct ColdClearSessionStats {
    pub rebuilds: u32,
    pub reuses: u32,
    pub queue_additions: u32,
    pub commits: u32,
    pub commit_misses: u32,
    pub resets: u32,
}

/// Stateful Cold Clear floor used by every controller. The mirror is
/// intentionally stricter than `BotState::reset`: any board/hold/B2B/combo or
/// queue-prefix mismatch discards the DAG instead of risking a stale root.
#[derive(Default)]
pub(crate) struct ColdClearSession {
    active: Option<PersistentColdClear>,
    stats: ColdClearSessionStats,
}

pub fn analyze_cold_clear(board: &Board, nodes: u32, incoming: u32) -> CcAnalysis {
    analyze_cold_clear_with_pc(board, nodes, incoming, 0, true)
}

fn analyze_cold_clear_with_pc(
    board: &Board,
    nodes: u32,
    incoming: u32,
    perfect_clear_special_attack: u32,
    can_hold: bool,
) -> CcAnalysis {
    let actions = legal_actions_with_hold(board, can_hold);
    if actions.is_empty() {
        return empty_analysis();
    }

    let target = nodes.max(32);
    seed_search(board, target, incoming);
    let evaluator = evaluator_for_pc(perfect_clear_special_attack);
    let mut state = BotState::new(board.clone(), options_for_target(target));
    analyze_existing_state(&mut state, &evaluator, &actions, target, incoming).0
}

fn empty_analysis() -> CcAnalysis {
    CcAnalysis {
        chosen: None,
        root_scores: HashMap::new(),
        nodes: 0,
    }
}

fn evaluator_for_pc(perfect_clear_special_attack: u32) -> Standard {
    let mut evaluator = Standard::default();
    // Offline PC0 experiments remove the bonus. Live simulator matches keep
    // Cold Clear Standard's normal perfect-clear preference.
    if perfect_clear_special_attack == 0 {
        evaluator.perfect_clear = 0;
    }
    evaluator.stack_pc_damage = true;
    evaluator
}

fn options_for_target(target: u32) -> Options {
    Options {
        mode: MovementMode::ZeroG,
        spawn_rule: SpawnRule::Row19Or20,
        use_hold: true,
        speculate: true,
        min_nodes: 0,
        max_nodes: target.saturating_mul(2),
        threads: 1,
    }
}

fn analyze_existing_state(
    state: &mut BotState<Standard>,
    evaluator: &Standard,
    actions: &[PlacementAction],
    target: u32,
    incoming: u32,
) -> (CcAnalysis, HashMap<ActionKey, libtetris::FallingPiece>) {
    let mut stalled = 0;
    while state.node_count() < target || !state.min_thinking_reached() {
        match state.think() {
            Ok(thinker) => {
                let result = thinker.think(evaluator);
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
    let mut commit_candidates = HashMap::new();
    for candidate in state.candidate_scores() {
        let candidate_key = ActionKey {
            placement: candidate.mv,
            hold: candidate.hold,
        };
        if let Some(action) = actions
            .iter()
            .find(|action| action.key() == candidate_key)
            .or_else(|| {
                actions
                    .iter()
                    .find(|action| same_action(action, candidate.mv, candidate.hold))
            })
        {
            root_scores.insert(action.key(), (candidate.value, candidate.spike));
            commit_candidates.insert(action.key(), candidate.mv);
        }
    }
    let chosen = state
        .suggest_move(evaluator, None, incoming)
        .and_then(|(mv, _)| {
            actions
                .iter()
                .find(|action| action.hold == mv.hold && action.placement == mv.expected_location)
                .or_else(|| {
                    actions
                        .iter()
                        .find(|action| same_action(action, mv.expected_location, mv.hold))
                })
                .cloned()
        })
        .or_else(|| safest_fallback(actions));

    (
        CcAnalysis {
            chosen,
            root_scores,
            nodes: state.node_count(),
        },
        commit_candidates,
    )
}

impl ColdClearSession {
    pub(crate) fn choose(
        &mut self,
        observation: &Observation,
        config: &AgentConfig,
    ) -> Option<SelectedAction> {
        let analysis = self.analyze(
            &observation.own.board,
            config.cold_clear_nodes,
            observation.own.incoming_total(),
            observation.rules.perfect_clear_special_attack,
            observation.own.can_hold,
        );
        selected_from_analysis(analysis)
    }

    fn analyze(
        &mut self,
        board: &Board,
        nodes: u32,
        incoming: u32,
        perfect_clear_special_attack: u32,
        can_hold: bool,
    ) -> CcAnalysis {
        let actions = legal_actions_with_hold(board, can_hold);
        if actions.is_empty() {
            if let Some(active) = &mut self.active {
                active.commit_candidates.clear();
            }
            return empty_analysis();
        }

        let target = nodes.max(32);
        let synced_queue_additions = self.active.as_mut().and_then(|active| {
            (active.target_nodes == target
                && active.perfect_clear_special_attack == perfect_clear_special_attack)
                .then(|| sync_observation(active, board))
                .flatten()
        });
        if let Some(added) = synced_queue_additions {
            self.stats.reuses = self.stats.reuses.saturating_add(1);
            self.stats.queue_additions = self.stats.queue_additions.saturating_add(added as u32);
        } else {
            self.active = Some(PersistentColdClear {
                state: BotState::new(board.clone(), options_for_target(target)),
                evaluator: evaluator_for_pc(perfect_clear_special_attack),
                mirror: board.clone(),
                target_nodes: target,
                perfect_clear_special_attack,
                commit_candidates: HashMap::new(),
            });
            self.stats.rebuilds = self.stats.rebuilds.saturating_add(1);
        }

        let active = self.active.as_mut().expect("session was initialized");
        // Native uses thread-local state and wasm32 uses a module-local atomic
        // stream. Reseed either implementation immediately before every
        // bounded think so Rayon work and stateless forecasts cannot perturb a
        // persistent floor, and both targets start from the same stable seed.
        seed_search(board, target, incoming);
        let (analysis, commit_candidates) = analyze_existing_state(
            &mut active.state,
            &active.evaluator,
            &actions,
            target,
            incoming,
        );
        active.commit_candidates = commit_candidates;
        analysis
    }

    /// Commit the move that was actually selected by the outer strategy. A
    /// non-CC override can reuse the DAG only when that exact root branch was
    /// expanded and its resulting public board state matches the real action.
    pub(crate) fn commit_selected(&mut self, selected: Option<&PlacementAction>) -> bool {
        let Some(selected) = selected else {
            if let Some(active) = &mut self.active {
                active.commit_candidates.clear();
            }
            return false;
        };

        let committed = self.active.as_mut().is_some_and(|active| {
            let Some(&dag_placement) = active.commit_candidates.get(&selected.key()) else {
                return false;
            };
            let mut dag_mirror = active.mirror.clone();
            let Some(dag_lock) = advance_mirror(
                &mut dag_mirror,
                dag_placement,
                selected.hold,
                selected.pieces_consumed,
            ) else {
                return false;
            };
            let mut actual_mirror = active.mirror.clone();
            let Some(actual_lock) = advance_mirror(
                &mut actual_mirror,
                selected.placement,
                selected.hold,
                selected.pieces_consumed,
            ) else {
                return false;
            };
            if dag_lock != selected.lock
                || actual_lock != selected.lock
                || !same_public_board(&dag_mirror, &actual_mirror)
                || !same_public_board(&actual_mirror, &selected.board_after)
            {
                return false;
            }

            active.state.advance_move(dag_placement);
            active.mirror = actual_mirror;
            active.commit_candidates.clear();
            true
        });

        if committed {
            self.stats.commits = self.stats.commits.saturating_add(1);
        } else {
            self.active = None;
            self.stats.commit_misses = self.stats.commit_misses.saturating_add(1);
        }
        committed
    }

    pub(crate) fn reset(&mut self) {
        self.active = None;
        self.stats.resets = self.stats.resets.saturating_add(1);
    }

    #[cfg(test)]
    fn stats(&self) -> ColdClearSessionStats {
        self.stats
    }

    #[cfg(test)]
    fn retained_nodes(&self) -> u32 {
        self.active
            .as_ref()
            .map_or(0, |active| active.state.node_count())
    }
}

fn sync_observation(active: &mut PersistentColdClear, observed: &Board) -> Option<usize> {
    if !same_board_without_queue(&active.mirror, observed) {
        return None;
    }
    let cached_queue: Vec<_> = active.mirror.next_queue().collect();
    let observed_queue: Vec<_> = observed.next_queue().collect();
    if observed_queue.len() < cached_queue.len()
        || observed_queue[..cached_queue.len()] != cached_queue
    {
        return None;
    }
    let suffix = &observed_queue[cached_queue.len()..];
    for &piece in suffix {
        active.state.add_next_piece(piece);
        active.mirror.add_next_piece(piece);
    }
    Some(suffix.len())
}

fn advance_mirror(
    board: &mut Board,
    placement: libtetris::FallingPiece,
    expected_hold: bool,
    expected_pieces_consumed: usize,
) -> Option<libtetris::LockResult> {
    let current = board.advance_queue()?;
    let mut used_hold = false;
    let mut pieces_consumed = 1;
    if current != placement.kind.0 {
        used_hold = true;
        let held = board.hold(current);
        let selected = match held {
            Some(piece) => piece,
            None => {
                pieces_consumed = 2;
                board.advance_queue()?
            }
        };
        if selected != placement.kind.0 {
            return None;
        }
    }
    if used_hold != expected_hold || pieces_consumed != expected_pieces_consumed {
        return None;
    }
    Some(board.lock_piece(placement))
}

fn same_board_without_queue(left: &Board, right: &Board) -> bool {
    left.get_field() == right.get_field()
        && left.hold_piece == right.hold_piece
        && left.b2b_bonus == right.b2b_bonus
        && left.combo == right.combo
}

fn same_public_board(left: &Board, right: &Board) -> bool {
    same_board_without_queue(left, right) && left.next_queue().eq(right.next_queue())
}

fn seed_search(board: &Board, nodes: u32, incoming: u32) {
    seed_deterministic_search(search_seed(board, nodes, incoming));
}

fn search_seed(board: &Board, nodes: u32, incoming: u32) -> u64 {
    // Explicit stable serialization/mixing keeps native and wasm32 on the
    // same seed contract; DefaultHasher's algorithm is not a public API.
    let mut seed = 0xA076_1D64_78BD_642F_u64;
    for row in board.get_field() {
        let mask = row.iter().enumerate().fold(0_u64, |bits, (x, occupied)| {
            bits | ((*occupied as u64) << x)
        });
        mix_search_seed(&mut seed, mask);
    }
    mix_search_seed(&mut seed, board.combo as u64);
    mix_search_seed(&mut seed, board.b2b_bonus as u64);
    mix_search_seed(
        &mut seed,
        board.hold_piece.map_or(0, |piece| piece as u64 + 1),
    );
    for piece in board.next_queue() {
        mix_search_seed(&mut seed, piece as u64 + 1);
    }
    // The remaining bag affects speculative continuations after the preview.
    let bag_mask = board
        .bag
        .iter()
        .fold(0_u64, |bits, piece| bits | (1_u64 << piece as u8));
    mix_search_seed(&mut seed, bag_mask);
    mix_search_seed(&mut seed, nodes as u64);
    mix_search_seed(&mut seed, incoming as u64);
    seed
}

fn mix_search_seed(seed: &mut u64, value: u64) {
    let mut mixed = seed.wrapping_add(value).wrapping_add(0x9E37_79B9_7F4A_7C15);
    mixed = (mixed ^ (mixed >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    mixed = (mixed ^ (mixed >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    *seed = mixed ^ (mixed >> 31);
}

#[cfg(not(target_arch = "wasm32"))]
pub fn choose_cold_clear(
    observation: &Observation,
    config: &AgentConfig,
) -> Option<SelectedAction> {
    let analysis = analyze_cold_clear_with_pc(
        &observation.own.board,
        config.cold_clear_nodes,
        observation.own.incoming_total(),
        observation.rules.perfect_clear_special_attack,
        observation.own.can_hold,
    );
    selected_from_analysis(analysis)
}

fn selected_from_analysis(analysis: CcAnalysis) -> Option<SelectedAction> {
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
        policy_override: super::PolicyOverride::None,
        strategy_event: super::StrategyEvent::None,
        strategy_detail: 0,
    })
}

pub fn forecast_opponent(observation: &Observation, config: &AgentConfig) -> OpponentForecast {
    let first_start_ms = match observation.opponent.phase {
        PhaseView::Ready => observation.now_ms,
        PhaseView::Moving { started_ms } => started_ms,
        PhaseView::LineClear { ends_ms } => ends_ms,
    };
    let mut board = observation.opponent.board.clone();
    let mut pending = cancellable_pending_at_first_lock(observation);
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
            step != 0 || observation.opponent.can_hold,
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

fn cancellable_pending_at_first_lock(observation: &Observation) -> u32 {
    let total = observation.opponent.incoming_total();
    match observation.opponent.phase {
        // The engine raises mature garbage at `ends_ms` before scheduling the
        // opponent's next piece. It therefore cannot cancel that garbage with
        // the first forecast placement, even though the observation still
        // contains the pre-rise queue while line clear is active.
        PhaseView::LineClear { ends_ms } => total.saturating_sub(matured_lines(
            &observation.opponent.incoming,
            ends_ms,
            observation.rules.garbage_grace_ms,
        )),
        PhaseView::Ready | PhaseView::Moving { .. } => total,
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
    use crate::agent::{AgentController, AgentKind, PlayerView};
    use crate::rules::Rules;
    use libtetris::Piece;
    use std::time::Instant;

    fn queued_board() -> Board {
        let mut board = Board::new();
        for _ in 0..4 {
            for piece in [
                Piece::I,
                Piece::O,
                Piece::T,
                Piece::S,
                Piece::Z,
                Piece::L,
                Piece::J,
            ] {
                board.add_next_piece(piece);
            }
        }
        board
    }

    fn observation(board: Board, perfect_clear_special_attack: u32) -> Observation {
        let mut rules = Rules::pinned();
        rules.perfect_clear_special_attack = perfect_clear_special_attack;
        let player = PlayerView {
            board,
            can_hold: true,
            incoming: Vec::new(),
            phase: PhaseView::Ready,
            pieces: 0,
            average_piece_ms: 300.0,
        };
        Observation {
            now_ms: 0,
            rules,
            own: player.clone(),
            opponent: player,
        }
    }

    fn config_with_nodes(nodes: u32) -> AgentConfig {
        AgentConfig {
            cold_clear_nodes: nodes,
            ..AgentConfig::default()
        }
    }

    #[test]
    fn line_clear_forecast_excludes_garbage_that_rises_before_next_piece() {
        let mut observed = observation(queued_board(), 0);
        observed.now_ms = 900;
        observed.opponent.incoming = vec![
            IncomingPacket {
                lines: 4,
                arrival_ms: 0,
            },
            IncomingPacket {
                lines: 3,
                arrival_ms: 600,
            },
        ];
        observed.opponent.phase = PhaseView::LineClear { ends_ms: 1_501 };
        assert_eq!(cancellable_pending_at_first_lock(&observed), 3);

        observed.opponent.phase = PhaseView::Ready;
        assert_eq!(cancellable_pending_at_first_lock(&observed), 7);
    }

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

    #[cfg(not(target_arch = "wasm32"))]
    #[test]
    fn persistent_search_is_deterministic_across_rayon_workers() {
        use rayon::prelude::*;

        let signatures: Vec<_> = (0..8_u32)
            .into_par_iter()
            .map(|worker_case| {
                let config = config_with_nodes(1_200);
                let mut session = ColdClearSession::default();
                let mut board = queued_board();
                let mut signature = Vec::new();
                let mut expanded_after_reuse = false;

                for ply in 0..12_u32 {
                    // A forecast or another match can consume the worker's
                    // thread-local deterministic RNG between retained moves.
                    // Vary that interference so this test fails if a later
                    // retained-tree expansion relies on leftover RNG state.
                    let mut noise = queued_board();
                    for garbage in 0..=(worker_case + ply) % 3 {
                        noise.add_garbage(((worker_case + garbage) % 10) as usize);
                    }
                    let _ = analyze_cold_clear(
                        &noise,
                        96 + worker_case * 17 + ply * 13,
                        worker_case + ply,
                    );

                    let before = session.retained_nodes();
                    let analysis = session.analyze(&board, config.cold_clear_nodes, 0, 10, true);
                    let after = session.retained_nodes();
                    expanded_after_reuse |= ply > 0 && after > before;
                    let action = analysis.chosen.expect("persistent move");
                    signature.push((action.key(), analysis.root_scores, analysis.nodes));
                    assert!(session.commit_selected(Some(&action)));

                    board = action.board_after.clone();
                    board.add_next_piece(
                        [
                            Piece::I,
                            Piece::O,
                            Piece::T,
                            Piece::S,
                            Piece::Z,
                            Piece::L,
                            Piece::J,
                        ][ply as usize % 7],
                    );
                }

                assert_eq!(session.stats().rebuilds, 1);
                assert_eq!(session.stats().reuses, 11);
                assert_eq!(session.stats().commits, 12);
                assert!(expanded_after_reuse, "retained DAG was never expanded");
                signature
            })
            .collect();

        let expected = signatures.first().expect("at least one signature");
        assert!(signatures.iter().all(|signature| signature == expected));
    }

    #[test]
    fn persistent_session_reuses_second_move_and_appends_queue() {
        let config = config_with_nodes(1_200);
        let first_observation = observation(queued_board(), 10);
        let mut session = ColdClearSession::default();

        let first = session
            .choose(&first_observation, &config)
            .expect("first move");
        assert_eq!(session.stats().rebuilds, 1);
        assert!(session.commit_selected(Some(&first.action)));
        assert!(session.retained_nodes() > 0);

        let mut second_board = first.action.board_after.clone();
        second_board.add_next_piece(Piece::S);
        let second = session.choose(&observation(second_board, 10), &config);

        assert!(second.is_some());
        assert_eq!(session.stats().rebuilds, 1);
        assert_eq!(session.stats().reuses, 1);
        assert_eq!(session.stats().queue_additions, 1);
        assert_eq!(session.stats().commits, 1);
    }

    #[test]
    fn expanded_non_cc_override_can_commit_and_reuse() {
        let config = config_with_nodes(1_500);
        let board = queued_board();
        let mut session = ColdClearSession::default();
        let suggested = session
            .choose(&observation(board.clone(), 10), &config)
            .expect("CC suggestion");
        let alternate = legal_actions(&board)
            .into_iter()
            .find(|action| {
                action.key() != suggested.action.key()
                    && session
                        .active
                        .as_ref()
                        .is_some_and(|active| active.commit_candidates.contains_key(&action.key()))
            })
            .expect("an expanded alternative root branch");

        assert!(session.commit_selected(Some(&alternate)));
        let mut next_board = alternate.board_after.clone();
        next_board.add_next_piece(Piece::Z);
        assert!(session
            .choose(&observation(next_board, 10), &config)
            .is_some());
        assert_eq!(session.stats().commits, 1);
        assert_eq!(session.stats().reuses, 1);
        assert_eq!(session.stats().rebuilds, 1);
    }

    #[test]
    fn unsupported_override_is_discarded_and_rebuilt() {
        let config = config_with_nodes(800);
        let board = queued_board();
        let mut session = ColdClearSession::default();
        let mut selected = session
            .choose(&observation(board.clone(), 10), &config)
            .expect("CC suggestion")
            .action;
        selected.board_after.combo = selected.board_after.combo.saturating_add(1);

        assert!(!session.commit_selected(Some(&selected)));
        assert_eq!(session.stats().commit_misses, 1);
        assert!(session.choose(&observation(board, 10), &config).is_some());
        assert_eq!(session.stats().rebuilds, 2);
    }

    #[test]
    fn observation_state_mismatch_rebuilds_instead_of_reusing() {
        let config = config_with_nodes(800);
        let mut session = ColdClearSession::default();
        let first = session
            .choose(&observation(queued_board(), 10), &config)
            .expect("CC suggestion");
        assert!(session.commit_selected(Some(&first.action)));

        let mut mismatched = first.action.board_after.clone();
        mismatched.add_next_piece(Piece::T);
        mismatched.b2b_bonus = !mismatched.b2b_bonus;
        assert!(session
            .choose(&observation(mismatched, 10), &config)
            .is_some());
        assert_eq!(session.stats().rebuilds, 2);
        assert_eq!(session.stats().reuses, 0);
    }

    #[test]
    fn reuse_guard_checks_field_hold_b2b_and_combo() {
        let board = queued_board();

        let mut garbage_changed = board.clone();
        garbage_changed.add_garbage(3);
        assert!(!same_board_without_queue(&board, &garbage_changed));

        let mut hold_changed = board.clone();
        hold_changed.hold_piece = Some(Piece::T);
        assert!(!same_board_without_queue(&board, &hold_changed));

        let mut b2b_changed = board.clone();
        b2b_changed.b2b_bonus = true;
        assert!(!same_board_without_queue(&board, &b2b_changed));

        let mut combo_changed = board.clone();
        combo_changed.combo = 1;
        assert!(!same_board_without_queue(&board, &combo_changed));
    }

    #[test]
    fn controller_reset_discards_nested_floor_state() {
        let config = config_with_nodes(800);
        let first_observation = observation(queued_board(), 10);
        let mut controller = AgentController::new(
            AgentKind::KasaneGuard,
            AgentConfig {
                enable_tempo: false,
                ..config
            },
        )
        .expect("guard controller");

        assert!(controller.choose(&first_observation).is_some());
        let before_reset = controller
            .cold_clear_fallback
            .as_ref()
            .and_then(|floor| floor.cold_clear_session.as_ref())
            .expect("nested CC session")
            .stats();
        assert_eq!(before_reset.commits, 1);
        assert_eq!(before_reset.rebuilds, 1);

        controller.reset();
        assert!(controller.choose(&first_observation).is_some());
        let after_reset = controller
            .cold_clear_fallback
            .as_ref()
            .and_then(|floor| floor.cold_clear_session.as_ref())
            .expect("nested CC session")
            .stats();
        assert_eq!(after_reset.resets, 1);
        assert_eq!(after_reset.rebuilds, 2);
        assert_eq!(after_reset.commits, 2);
    }

    #[test]
    fn no_legal_move_returns_none_without_creating_a_session() {
        let config = config_with_nodes(800);
        let mut session = ColdClearSession::default();
        assert!(session
            .choose(&observation(Board::new(), 10), &config)
            .is_none());
        assert!(session.active.is_none());
        assert_eq!(session.stats(), ColdClearSessionStats::default());
    }

    #[test]
    fn pc0_and_pc10_keep_the_expected_evaluators() {
        let pc0 = evaluator_for_pc(0);
        let pc10 = evaluator_for_pc(10);
        let standard = Standard::default();
        assert_eq!(pc0.perfect_clear, 0);
        assert!(pc0.stack_pc_damage);
        assert_eq!(pc10.perfect_clear, standard.perfect_clear);
        assert!(pc10.stack_pc_damage);
    }

    #[test]
    #[ignore = "120k-node release benchmark"]
    fn persistent_120k_second_move_benchmark() {
        let config = config_with_nodes(120_000);
        let first_observation = observation(queued_board(), 10);
        let mut session = ColdClearSession::default();

        let started = Instant::now();
        let first = session
            .choose(&first_observation, &config)
            .expect("first 120k move");
        let first_elapsed = started.elapsed();
        assert!(session.commit_selected(Some(&first.action)));
        let retained_after_commit = session.retained_nodes();

        let mut second_board = first.action.board_after.clone();
        second_board.add_next_piece(Piece::L);
        let second_observation = observation(second_board, 10);
        let started = Instant::now();
        let persistent_second = session.choose(&second_observation, &config);
        let persistent_elapsed = started.elapsed();

        let started = Instant::now();
        let stateless_second = choose_cold_clear(&second_observation, &config);
        let stateless_elapsed = started.elapsed();

        assert!(persistent_second.is_some());
        assert!(stateless_second.is_some());
        assert_eq!(session.stats().reuses, 1);
        eprintln!(
            "cc120k first_ms={:.3} persistent_second_ms={:.3} stateless_second_ms={:.3} speedup={:.2}x retained_after_commit={} retained_after_second={}",
            first_elapsed.as_secs_f64() * 1_000.0,
            persistent_elapsed.as_secs_f64() * 1_000.0,
            stateless_elapsed.as_secs_f64() * 1_000.0,
            stateless_elapsed.as_secs_f64() / persistent_elapsed.as_secs_f64().max(f64::EPSILON),
            retained_after_commit,
            session.retained_nodes(),
        );
    }
}
