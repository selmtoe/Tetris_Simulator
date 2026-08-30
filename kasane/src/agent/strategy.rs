//! Mixture-of-experts v2 strategy layer.
//!
//! Equal-node Cold Clear is the ordinary/survival floor. The proven legacy
//! Base-v3 + strict Tempo policy is retained as a finishing expert. Two
//! cancellation-dodge mechanisms are intentionally separate: hold-fire keeps
//! an already fireable action and delays its lock, while charge-then-release
//! places multiple non-firing pieces and may release only after an observed
//! incoming/risen-garbage edge.

use super::guard::{project_action_safety, safety_gate_accepts, ActionSafety};
use super::kasane::{choose_kasane, project_timing, TimingProjection};
use super::{
    forecast_opponent, matured_lines, packet_lines_after_cancel, AgentConfig, Intent, Observation,
    OpponentForecast, PolicyOverride, SelectedAction, StrategyEvent,
};
use crate::base::{base_features, BaseModel};
use crate::model::{
    BoardGeometry, TempoModel, FEATURE_NAMES, STRATEGY_EXTRA_FEATURE_NAMES,
    STRATEGY_STATE_FEATURE_NAMES,
};
use crate::search::{
    attack_with_pc, legal_actions, legal_actions_with_hold, ActionKey, PlacementAction,
};
use libtetris::{Board, Piece};
use std::cmp::Ordering;
use std::collections::HashMap;
use std::sync::OnceLock;

static LEGACY_ATTACK_MODEL: OnceLock<TempoModel> = OnceLock::new();

#[derive(Clone, Copy, Debug, Default)]
struct FutureResources {
    action_count: u32,
    clear_count: u32,
    attack_count: u32,
    max_attack: u32,
    mean_top_attack: f32,
    combo_continuations: u32,
    combo_max_attack: u32,
}

#[derive(Clone)]
struct CandidateContext {
    base_value: f32,
    base_spike: f32,
    geometry: BoardGeometry,
    future: FutureResources,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
enum StrategyPhase {
    #[default]
    Neutral,
    HoldFire,
    Charge,
    Armed,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
enum ExpertMode {
    #[default]
    Survival,
    Attack,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct IncomingObservation {
    queued_lines: u32,
    risen_lines: u32,
}

impl IncomingObservation {
    fn lines(self) -> u32 {
        self.queued_lines.saturating_add(self.risen_lines)
    }

    fn is_edge(self) -> bool {
        self.lines() > 0
    }
}

#[derive(Clone, Debug, Default)]
pub(crate) struct StrategyMemory {
    phase: StrategyPhase,
    expert_mode: ExpertMode,
    charge_started_ms: u64,
    charge_pieces: u32,
    last_seen_arrival_ms: Option<u64>,
    last_incoming_total: u32,
    expected_post_action_blocks: Option<u32>,
    /// A garbage-rise edge can occur on a piece whose placements cannot
    /// clear. Keep the offensive release armed for the following piece(s)
    /// instead of forgetting the one observable edge immediately.
    release_pending_until_ms: u64,
    ren_committed: bool,
}

impl StrategyMemory {
    fn observe_incoming(&mut self, observation: &Observation) -> IncomingObservation {
        let newest = observation
            .own
            .incoming
            .iter()
            .map(|packet| packet.arrival_ms)
            .max();
        let by_arrival = newest
            .filter(|arrival| {
                self.last_seen_arrival_ms
                    .map(|seen| *arrival > seen)
                    .unwrap_or(true)
            })
            .map(|arrival| {
                observation
                    .own
                    .incoming
                    .iter()
                    .filter(|packet| packet.arrival_ms == arrival)
                    .map(|packet| packet.lines)
                    .sum()
            })
            .unwrap_or(0);
        let total = observation.own.incoming_total();
        let queued_lines = by_arrival.max(total.saturating_sub(self.last_incoming_total));
        // A long hold may let a packet rise before the controller is queried
        // again. The previous selected board is the exact pre-rise board, and
        // every garbage row contributes nine occupied cells.
        let current_blocks = BoardGeometry::measure(&observation.own.board).blocks;
        let risen_lines = self
            .expected_post_action_blocks
            .map(|expected| current_blocks.saturating_sub(expected) / 9)
            .unwrap_or(0);
        self.last_seen_arrival_ms = newest.or(self.last_seen_arrival_ms);
        self.last_incoming_total = total;
        IncomingObservation {
            queued_lines,
            risen_lines,
        }
    }

    fn reset_tactic(&mut self) {
        self.phase = StrategyPhase::Neutral;
        self.charge_started_ms = 0;
        self.charge_pieces = 0;
        self.release_pending_until_ms = 0;
        self.ren_committed = false;
    }

    fn finish_previous_hold_fire(&mut self) {
        if self.phase == StrategyPhase::HoldFire {
            self.phase = StrategyPhase::Neutral;
        }
    }

    fn note_selected(&mut self, selected: &SelectedAction) {
        self.expected_post_action_blocks =
            Some(BoardGeometry::measure(&selected.action.board_after).blocks);
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SelectionMode {
    Neutral,
    Charge,
    ChargeReleaseIncomingEdge,
    ChargeReleaseGarbageRiseEdge,
}

#[derive(Clone, Copy, Debug, Default)]
struct StateSignals {
    incoming_edge: IncomingObservation,
    opponent_attack_eta_ms: u64,
    immediate_attack_options: u32,
    safe_nonfire_options: u32,
    max_immediate_attack: u32,
    charge_safety_score: f32,
}

pub(crate) fn choose_strategy_v2(
    observation: &Observation,
    config: &AgentConfig,
    base_model: &BaseModel,
    model: &TempoModel,
    fallback: Option<SelectedAction>,
    memory: &mut StrategyMemory,
) -> Option<SelectedAction> {
    let mut fallback = fallback?;
    if !model.is_strategy_v2() {
        return Some(fallback);
    }

    memory.finish_previous_hold_fire();
    let mut actions = legal_actions_with_hold(&observation.own.board, observation.own.can_hold);
    let fallback_key = fallback.action.key();
    if !actions.iter().any(|action| action.key() == fallback_key) {
        return Some(fallback);
    }
    // PC is ordinary live-match firepower when the simulator enables it, but
    // remains reward-zero in the pinned training/cheese contract. Preserve a
    // real CC PC instead of letting a PC0-trained placement expert spoil it.
    if observation.rules.perfect_clear_special_attack > 0 && fallback.action.lock.perfect_clear {
        fallback.policy_override = PolicyOverride::None;
        fallback.strategy_event = StrategyEvent::SurvivalExpert;
        apply_strategy_event(memory, fallback.strategy_event, observation.now_ms);
        memory.note_selected(&fallback);
        return Some(fallback);
    }
    let forecast = forecast_opponent(observation, config);
    let own_before = BoardGeometry::measure(&observation.own.board);
    let opponent_geometry = BoardGeometry::measure(&observation.opponent.board);
    let incoming_edge = memory.observe_incoming(observation);
    let signals = state_signals(
        observation,
        config,
        &actions,
        &forecast,
        own_before,
        opponent_geometry,
        incoming_edge,
    );
    let previous_phase = memory.phase;
    let mode = selection_mode(
        observation,
        config,
        memory,
        signals,
        own_before,
        opponent_geometry,
    );

    let baseline_lock_ms = lock_ms(observation, &fallback.action, fallback.wait_ms);
    let baseline_projection = project_timing(
        observation,
        &fallback.action,
        &forecast,
        baseline_lock_ms,
        lock_ms(observation, &fallback.action, 0),
    );
    let baseline_safety = project_action_safety(observation, &fallback);
    let mut contexts = HashMap::<ActionKey, CandidateContext>::new();
    let baseline_context =
        candidate_context(observation, base_model, &fallback.action, &mut contexts);
    let baseline_features = strategy_features(
        observation,
        &forecast,
        &own_before,
        &opponent_geometry,
        &fallback.action,
        fallback.wait_ms,
        baseline_lock_ms,
        baseline_projection,
        baseline_safety,
        &baseline_context,
        &baseline_context,
        baseline_projection,
        baseline_safety,
        true,
        memory,
        signals,
        0.0,
    );
    let baseline_score = model.score(&baseline_features);

    if mode == SelectionMode::Neutral && config.strategy_enable_ren {
        if let Some(selected) = choose_ren_expert(
            observation,
            config,
            &actions,
            &forecast,
            own_before,
            opponent_geometry,
            &fallback,
            baseline_safety,
            memory,
        ) {
            apply_strategy_event(memory, selected.strategy_event, observation.now_ms);
            memory.note_selected(&selected);
            return Some(selected);
        }
    }

    // Charge has priority once actually entered so a finish-context transition
    // cannot silently turn a multi-piece plan into a forecast-only release.
    // A merely eligible new charge does not pre-empt the MoE gate: if Basic is
    // already a strong finisher, use it and retain raw throughput.
    if !matches!(previous_phase, StrategyPhase::Charge | StrategyPhase::Armed)
        && config.strategy_enable_moe
    {
        if let Some(selected) = choose_attack_expert(
            observation,
            config,
            base_model,
            model,
            &forecast,
            own_before,
            opponent_geometry,
            &fallback,
            baseline_projection,
            baseline_safety,
            &baseline_context,
            baseline_score,
            memory,
            signals,
            &mut contexts,
        ) {
            apply_strategy_event(memory, selected.strategy_event, observation.now_ms);
            memory.note_selected(&selected);
            return Some(selected);
        }
    }
    memory.expert_mode = ExpertMode::Survival;

    // Rank cheaply before computing next-ply resource summaries.  The exact CC
    // action is retained regardless of rank; alternatives are deliberately
    // capped because the CC floor already covers ordinary stacking.
    actions.sort_by(|left, right| {
        quick_rank(observation, base_model, right)
            .partial_cmp(&quick_rank(observation, base_model, left))
            .unwrap_or(Ordering::Equal)
    });
    let fallback_raw = attack_with_pc(
        &fallback.action.lock,
        observation.rules.perfect_clear_special_attack,
    );
    let charge_diversion = should_divert_charge(
        mode,
        previous_phase,
        fallback_raw,
        observation.own.incoming_total(),
    );
    let alternative_limit = if mode == SelectionMode::Charge {
        // Follow the exact CC floor while it is naturally non-firing. Only
        // divert from a fireable floor after a charge is established and a
        // packet is already queued. The candidate below must prove that this
        // exact non-clearing lock raises the packet; forecast-only diversion is
        // forbidden. The stored attack is then released on the observed board
        // rise edge on the following piece.
        if charge_diversion {
            config.strategy_max_alternatives
        } else {
            0
        }
    } else {
        config.strategy_max_alternatives
    };
    let mut selected_actions = Vec::with_capacity(alternative_limit + 1);
    if let Some(action) = actions.iter().find(|action| action.key() == fallback_key) {
        selected_actions.push(action.clone());
    }
    selected_actions.extend(
        actions
            .into_iter()
            .filter(|action| action.key() != fallback_key)
            .filter(|action| {
                !charge_diversion
                    || (action.lock.cleared_lines.is_empty()
                        && attack_with_pc(
                            &action.lock,
                            observation.rules.perfect_clear_special_attack,
                        ) == 0)
            })
            .take(alternative_limit),
    );

    let mut best: Option<SelectedAction> = None;

    for action in selected_actions {
        let same_placement = action.key() == fallback_key;
        let base_lock_ms = lock_ms(observation, &action, 0);
        let context = candidate_context(observation, base_model, &action, &mut contexts);
        let raw_attack =
            attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
        if (mode == SelectionMode::Charge && raw_attack > 0)
            || (matches!(
                mode,
                SelectionMode::ChargeReleaseIncomingEdge
                    | SelectionMode::ChargeReleaseGarbageRiseEdge
            ) && raw_attack == 0)
        {
            continue;
        }
        if mode == SelectionMode::Charge
            && !same_placement
            && (!charge_diversion
                || !action.lock.cleared_lines.is_empty()
                || context.future.max_attack < 4
                || context.future.attack_count == 0)
        {
            continue;
        }
        if mode == SelectionMode::Charge
            && !same_placement
            && previous_phase == StrategyPhase::Neutral
            && !charge_resource_improves(&context, &baseline_context)
        {
            continue;
        }
        let waits = match mode {
            SelectionMode::Charge => vec![0],
            SelectionMode::ChargeReleaseIncomingEdge
            | SelectionMode::ChargeReleaseGarbageRiseEdge => vec![0],
            SelectionMode::Neutral => strategy_wait_candidates(
                observation,
                &action,
                &forecast,
                base_lock_ms,
                config.maximum_wait_ms.min(config.strategy_max_wait_ms),
            ),
        };
        for wait_ms in waits {
            if same_placement && wait_ms == fallback.wait_ms {
                continue;
            }
            let candidate_lock_ms = base_lock_ms.saturating_add(wait_ms);
            let projection = project_timing(
                observation,
                &action,
                &forecast,
                candidate_lock_ms,
                base_lock_ms,
            );
            if charge_diversion && !same_placement && projection.rise == 0 {
                continue;
            }
            let intent = classify_strategy_intent(
                observation,
                &action,
                wait_ms,
                projection,
                baseline_projection,
                own_before,
            );
            let strategy_event = candidate_strategy_event(
                mode,
                previous_phase,
                config,
                memory,
                &context,
                wait_ms,
                projection,
                baseline_projection,
            );
            let candidate_shell = SelectedAction {
                action: action.clone(),
                wait_ms,
                intent,
                score: 0.0,
                policy_override: classify_override(
                    same_placement,
                    wait_ms,
                    projection,
                    baseline_projection,
                ),
                strategy_event,
                strategy_detail: 0,
            };
            let safety = project_action_safety(observation, &candidate_shell);

            if same_placement {
                if !safe_same_placement_wait(
                    safety,
                    baseline_safety,
                    projection,
                    baseline_projection,
                ) || (wait_ms > 0 && !timing_improves(projection, baseline_projection))
                {
                    continue;
                }
            } else {
                let safe_charge_tank = mode == SelectionMode::Charge
                    && observation.own.incoming_total() > 0
                    && safety.headroom_after_rise >= config.strategy_charge_min_headroom;
                let gate_accepts = if charge_diversion {
                    charge_diversion_safety_accepts(safety, own_before, config)
                } else if matches!(
                    mode,
                    SelectionMode::Charge
                        | SelectionMode::ChargeReleaseIncomingEdge
                        | SelectionMode::ChargeReleaseGarbageRiseEdge
                ) {
                    charge_safety_gate_accepts(safety, baseline_safety, config, safe_charge_tank)
                } else {
                    safety_gate_accepts(observation, &candidate_shell, &fallback, config)
                };
                if !gate_accepts
                    || (!safe_charge_tank && projection.rise > baseline_projection.rise)
                {
                    continue;
                }
            }

            let features = strategy_features(
                observation,
                &forecast,
                &own_before,
                &opponent_geometry,
                &action,
                wait_ms,
                candidate_lock_ms,
                projection,
                safety,
                &context,
                &baseline_context,
                baseline_projection,
                baseline_safety,
                same_placement,
                memory,
                signals,
                resource_score(&context) - resource_score(&baseline_context),
            );
            let score = if mode == SelectionMode::Charge {
                // During a committed charge an attacking CC floor is not a
                // meaningful score baseline: comparing against it caused an
                // armed plan to fire just before the predicted packet. Rank
                // safe non-firing placements by stored next-ply resources.
                model.score(&features)
                    + resource_score(&context) * 2.0
                    + if charge_diversion {
                        // If a packet is already due, prefer the safe lock that
                        // actually raises it. This converts the following shot
                        // from cancellation into sent pressure.
                        projection.rise as f32 * 1_000.0
                    } else {
                        0.0
                    }
            } else {
                model.score(&features)
            };
            let is_observed_release = matches!(
                mode,
                SelectionMode::ChargeReleaseIncomingEdge
                    | SelectionMode::ChargeReleaseGarbageRiseEdge
            );
            let required_advantage = if is_observed_release || mode == SelectionMode::Charge {
                f32::NEG_INFINITY
            } else {
                config.strategy_min_advantage
                    + if same_placement {
                        0.0
                    } else {
                        config.strategy_placement_min_advantage
                    }
            };
            if score < baseline_score + required_advantage {
                continue;
            }
            let candidate = SelectedAction {
                // Once an actual edge releases an armed charge, prefer attack
                // that escapes cancellation (sent) over merely cancelling.
                score: score
                    + if is_observed_release {
                        projection.sent as f32 * 100.0 - projection.cancelled as f32 * 8.0
                    } else {
                        0.0
                    },
                ..candidate_shell
            };
            if best
                .as_ref()
                .map(|current| candidate.score > current.score)
                .unwrap_or(true)
            {
                best = Some(candidate);
            }
        }
    }
    if let Some(selected) = best {
        apply_strategy_event(memory, selected.strategy_event, observation.now_ms);
        memory.note_selected(&selected);
        return Some(selected);
    }

    // Following a non-firing CC floor is itself a valid charging step, but it
    // is deliberately not counted as a placement/wait override.
    fallback.score = baseline_score;
    fallback.policy_override = PolicyOverride::None;
    fallback.strategy_event = match mode {
        SelectionMode::Charge if fallback_raw == 0 => candidate_strategy_event(
            mode,
            previous_phase,
            config,
            memory,
            &baseline_context,
            0,
            baseline_projection,
            baseline_projection,
        ),
        SelectionMode::ChargeReleaseIncomingEdge if fallback_raw > 0 => {
            StrategyEvent::ChargeReleaseIncomingEdge
        }
        SelectionMode::ChargeReleaseGarbageRiseEdge if fallback_raw > 0 => {
            StrategyEvent::ChargeReleaseGarbageRiseEdge
        }
        _ if matches!(previous_phase, StrategyPhase::Charge | StrategyPhase::Armed) => {
            StrategyEvent::ChargeAbort
        }
        _ if fallback.wait_ms > 0 && fallback_raw > 0 => StrategyEvent::SurvivalExpertHoldFire,
        _ => StrategyEvent::SurvivalExpert,
    };
    apply_strategy_event(memory, fallback.strategy_event, observation.now_ms);
    memory.note_selected(&fallback);
    Some(fallback)
}

#[allow(clippy::too_many_arguments)]
fn choose_attack_expert(
    observation: &Observation,
    config: &AgentConfig,
    base_model: &BaseModel,
    model: &TempoModel,
    forecast: &OpponentForecast,
    own_before: BoardGeometry,
    opponent: BoardGeometry,
    cc_floor: &SelectedAction,
    cc_projection: TimingProjection,
    cc_safety: ActionSafety,
    cc_context: &CandidateContext,
    cc_score: f32,
    memory: &mut StrategyMemory,
    signals: StateSignals,
    contexts: &mut HashMap<ActionKey, CandidateContext>,
) -> Option<SelectedAction> {
    // The placement expert was trained/calibrated against a 300-node floor.
    // At production budgets Cold Clear's retained 120k-node DAG is a materially
    // stronger placement oracle; replacing it reduced browser throughput to
    // 0.262 attack/move. Keep the learned timing/REN/release layers, but do not
    // run this lower-budget placement expert over a high-confidence floor.
    if config.cold_clear_nodes >= 10_000 {
        return None;
    }
    let hole_burden = opponent.holes.saturating_add(opponent.covered / 2);
    // Height alone is not enough to justify abandoning the equal-node CC
    // floor. A clean high stack can downstack efficiently; covered garbage or
    // queued pressure is what makes the opponent tactically constrained. This
    // keeps the bottom-12 cheese finisher, while avoiding long low-throughput
    // attack-expert excursions in ordinary flat play.
    let constrained = hole_burden >= config.strategy_attack_hole_burden
        || observation.opponent.incoming_total() >= 4;
    let hard_finish = opponent.max_height >= config.strategy_attack_hard_finish_height
        && (constrained || opponent.max_height >= 15);
    let vulnerable =
        constrained || opponent.max_height >= config.strategy_attack_enter_height.saturating_add(3);
    let staying = memory.expert_mode == ExpertMode::Attack
        && vulnerable
        && opponent.max_height >= config.strategy_attack_stay_height;
    let forecast_attack = forecast
        .events
        .iter()
        .filter(|event| {
            event.lock_ms
                <= observation
                    .now_ms
                    .saturating_add(config.strategy_release_window_ms)
        })
        .map(|event| event.raw_attack)
        .sum::<u32>();
    let forecast_quiet = forecast_attack <= 3;
    let due_1000 = matured_lines(
        &observation.own.incoming,
        observation.now_ms.saturating_add(1_000),
        observation.rules.garbage_grace_ms,
    );
    let own_headroom = 20_i32 - own_before.max_height as i32 - due_1000 as i32;
    if !(hard_finish || staying || (vulnerable && forecast_quiet))
        || own_headroom < config.strategy_attack_min_headroom
        || due_1000 > config.strategy_attack_max_due_1000
    {
        return None;
    }

    // This is the exact shipped Basic expert used by the +16.41 pp bottom-12
    // result: Base-v3 loaded by the controller, strict legacy Tempo weights,
    // charge/tank disabled and the original 2 s timing horizon.
    let mut attack_config = config.clone();
    attack_config.strict_base_policy = true;
    attack_config.enable_charge = false;
    attack_config.enable_tank = false;
    attack_config.maximum_wait_ms = 2_000;
    attack_config.base_guard_margin = 6.0;
    let legacy = LEGACY_ATTACK_MODEL.get_or_init(TempoModel::default);
    let mut selected = choose_kasane(observation, &attack_config, base_model, legacy)?;
    let candidate_safety = project_action_safety(observation, &selected);
    if candidate_safety.locked_out
        || candidate_safety.headroom_after_rise < config.strategy_attack_min_headroom
        || candidate_safety.rise > config.strategy_attack_max_due_1000
    {
        return None;
    }

    let context = candidate_context(observation, base_model, &selected.action, contexts);
    let base_lock_ms = lock_ms(observation, &selected.action, 0);
    let selected_lock_ms = base_lock_ms.saturating_add(selected.wait_ms);
    let projection = project_timing(
        observation,
        &selected.action,
        forecast,
        selected_lock_ms,
        base_lock_ms,
    );
    let features = strategy_features(
        observation,
        forecast,
        &own_before,
        &opponent,
        &selected.action,
        selected.wait_ms,
        selected_lock_ms,
        projection,
        candidate_safety,
        &context,
        cc_context,
        cc_projection,
        cc_safety,
        selected.action.key() == cc_floor.action.key(),
        memory,
        signals,
        resource_score(&context) - resource_score(cc_context),
    );
    let expert_score = model.score(&features);
    let same_placement = selected.action.key() == cc_floor.action.key();
    // Entering the attack expert requires the calibrated conservative gate.
    // Once a genuinely constrained high board has already triggered it, keep
    // enough hysteresis to finish the downstack instead of handing tempo back
    // as soon as the opponent drops below the hard-finish height.
    let placement_advantage = if staying {
        config.strategy_placement_min_advantage.min(6.0)
    } else {
        config.strategy_placement_min_advantage
    };
    let required_advantage = config.strategy_min_advantage
        + if same_placement {
            0.0
        } else {
            placement_advantage
        };
    if !hard_finish && expert_score < cc_score + required_advantage {
        return None;
    }

    // Preserve Basic's placement/raw attack. When safe, move only the lock to
    // the opponent's exact fire timestamp so both new packets bypass old-queue
    // cancellation and Basic's raw output becomes sent pressure.
    let raw_attack = attack_with_pc(
        &selected.action.lock,
        observation.rules.perfect_clear_special_attack,
    );
    let mut selected_projection = projection;
    if selected.wait_ms == 0 && raw_attack > 0 && vulnerable {
        let immediate_safety = candidate_safety;
        let mut best_hold: Option<(SelectedAction, TimingProjection)> = None;
        for wait_ms in strategy_wait_candidates(
            observation,
            &selected.action,
            forecast,
            base_lock_ms,
            config.strategy_max_wait_ms,
        )
        .into_iter()
        .filter(|wait| *wait > 0)
        {
            let lock_ms = base_lock_ms.saturating_add(wait_ms);
            let hold_projection = project_timing(
                observation,
                &selected.action,
                forecast,
                lock_ms,
                base_lock_ms,
            );
            if !hold_projection.fires_simultaneously
                || hold_projection.sent <= projection.sent
                || hold_projection.cancelled > projection.cancelled
            {
                continue;
            }
            let mut hold = selected.clone();
            hold.wait_ms = wait_ms;
            hold.intent = Intent::CancellationDodge;
            let hold_safety = project_action_safety(observation, &hold);
            if !safe_same_placement_wait(hold_safety, immediate_safety, hold_projection, projection)
                || hold_safety.headroom_after_rise < config.strategy_attack_min_headroom
            {
                continue;
            }
            hold.score = expert_score + hold_projection.sent as f32 * 10.0;
            if best_hold
                .as_ref()
                .map(|(current, current_projection)| {
                    (hold_projection.sent, hold.score, std::cmp::Reverse(wait_ms))
                        > (
                            current_projection.sent,
                            current.score,
                            std::cmp::Reverse(current.wait_ms),
                        )
                })
                .unwrap_or(true)
            {
                best_hold = Some((hold, hold_projection));
            }
        }
        if let Some((hold, hold_projection)) = best_hold {
            selected = hold;
            selected_projection = hold_projection;
        }
    }

    selected.policy_override = classify_override(
        same_placement,
        selected.wait_ms,
        selected_projection,
        cc_projection,
    );
    selected.strategy_event = if selected.wait_ms > 0 && raw_attack > 0 {
        StrategyEvent::AttackExpertHoldFire
    } else {
        StrategyEvent::AttackExpert
    };
    selected.score = expert_score;
    memory.expert_mode = ExpertMode::Attack;
    Some(selected)
}

fn state_signals(
    observation: &Observation,
    config: &AgentConfig,
    actions: &[PlacementAction],
    forecast: &OpponentForecast,
    own: BoardGeometry,
    _opponent: BoardGeometry,
    incoming_edge: IncomingObservation,
) -> StateSignals {
    let mut immediate_attack_options = 0;
    let mut safe_nonfire_options = 0;
    let mut max_immediate_attack = 0;
    for action in actions {
        let attack = attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
        if attack > 0 && !action.lock.locked_out {
            immediate_attack_options += 1;
            max_immediate_attack = max_immediate_attack.max(attack);
        } else if !action.lock.locked_out {
            let geometry = BoardGeometry::measure(&action.board_after);
            let headroom = 20_i32 - geometry.max_height as i32;
            if headroom >= config.strategy_charge_min_headroom
                && geometry.holes <= own.holes
                && geometry.covered <= own.covered.saturating_add(1)
                && geometry.accessible_holes.saturating_add(1) >= own.accessible_holes
            {
                safe_nonfire_options += 1;
            }
        }
    }
    let opponent_attack_eta_ms = forecast
        .events
        .iter()
        .filter(|event| event.raw_attack > 0 && event.lock_ms >= observation.now_ms)
        .map(|event| event.lock_ms - observation.now_ms)
        .min()
        .unwrap_or(u64::MAX);
    let due_soon = matured_lines(
        &observation.own.incoming,
        observation.now_ms.saturating_add(500),
        observation.rules.garbage_grace_ms,
    );
    let charge_safety_score = (20_i32 - own.max_height as i32 - due_soon as i32) as f32
        - own.holes as f32 * 0.8
        - own.covered as f32 * 0.25;
    StateSignals {
        incoming_edge,
        opponent_attack_eta_ms,
        immediate_attack_options,
        safe_nonfire_options,
        max_immediate_attack,
        charge_safety_score,
    }
}

fn selection_mode(
    observation: &Observation,
    config: &AgentConfig,
    memory: &mut StrategyMemory,
    signals: StateSignals,
    own: BoardGeometry,
    opponent: BoardGeometry,
) -> SelectionMode {
    if !config.strategy_enable_stateful {
        return SelectionMode::Neutral;
    }
    let due_1000 = matured_lines(
        &observation.own.incoming,
        observation.now_ms.saturating_add(1_000),
        observation.rules.garbage_grace_ms,
    );
    let headroom_after_due = 20_i32 - own.max_height as i32 - due_1000 as i32;
    let active_charge = matches!(memory.phase, StrategyPhase::Charge | StrategyPhase::Armed);
    let speed_budget_ms = (observation.own.average_piece_ms.max(100.0)
        * config.strategy_charge_max_pieces as f32)
        .ceil() as u64;
    let maximum_charge_ms = speed_budget_ms
        .saturating_add(observation.rules.garbage_grace_ms)
        .clamp(4_000, 10_000);
    let charge_too_long = active_charge
        && (memory.charge_pieces >= config.strategy_charge_max_pieces
            || observation.now_ms.saturating_sub(memory.charge_started_ms) > maximum_charge_ms);
    if active_charge {
        if signals.incoming_edge.risen_lines > 0 {
            memory.release_pending_until_ms = observation
                .now_ms
                .saturating_add(config.strategy_release_window_ms.max(1));
        }
        let release_pending = memory.release_pending_until_ms >= observation.now_ms
            && memory.release_pending_until_ms != 0;
        // Offensive B-path: only a board-rise edge authorizes the stored fire
        // after garbage has left the cancellation queue.
        if release_pending && signals.immediate_attack_options > 0 {
            return SelectionMode::ChargeReleaseGarbageRiseEdge;
        }
        // If the current piece cannot fire, preserve the observed edge for a
        // bounded following-piece window. This remains a non-firing charge
        // step and cannot release from opponent forecast alone.
        if release_pending && headroom_after_due >= 4 {
            return SelectionMode::Charge;
        }
        // A newly queued packet may be countered immediately only when it is
        // unsafe to tank. This is a separately measured emergency path.
        if signals.incoming_edge.queued_lines > 0
            && headroom_after_due < config.strategy_charge_min_headroom
            && signals.immediate_attack_options > 0
        {
            return SelectionMode::ChargeReleaseIncomingEdge;
        }
        if headroom_after_due < config.strategy_charge_min_headroom || charge_too_long {
            memory.reset_tactic();
            return SelectionMode::Neutral;
        }
        return SelectionMode::Charge;
    }
    let opponent_vulnerable = opponent.max_height >= config.strategy_attack_enter_height
        || opponent.holes + opponent.covered / 2 >= config.strategy_attack_hole_burden
        || observation.opponent.incoming_total() >= 4;
    // "No attack in the four-ply forecast" is not a release opportunity. Start
    // only when the CC floor is naturally in a non-firing/resource-building
    // phase (represented by no immediate shot at all). We then preserve that
    // floor instead of sacrificing an available attack or changing placement.
    let forecast_quiet = signals.opponent_attack_eta_ms != u64::MAX
        && signals.opponent_attack_eta_ms > config.strategy_release_window_ms;
    let can_charge = opponent_vulnerable
        && memory.expert_mode != ExpertMode::Attack
        && opponent.max_height < config.strategy_attack_hard_finish_height
        && forecast_quiet
        && observation.own.incoming_total() == 0
        && headroom_after_due >= config.strategy_charge_min_headroom
        && signals.safe_nonfire_options > 0
        && signals.immediate_attack_options == 0;
    if can_charge {
        SelectionMode::Charge
    } else {
        SelectionMode::Neutral
    }
}

fn charge_resource_improves(candidate: &CandidateContext, baseline: &CandidateContext) -> bool {
    resource_score(candidate) > resource_score(baseline) + 0.25
        || candidate.geometry.holes < baseline.geometry.holes
        || candidate.geometry.covered < baseline.geometry.covered
}

fn should_divert_charge(
    mode: SelectionMode,
    previous_phase: StrategyPhase,
    fallback_raw: u32,
    own_incoming: u32,
) -> bool {
    mode == SelectionMode::Charge
        && matches!(previous_phase, StrategyPhase::Charge | StrategyPhase::Armed)
        && fallback_raw > 0
        && own_incoming > 0
}

fn charge_safety_gate_accepts(
    candidate: ActionSafety,
    baseline: ActionSafety,
    config: &AgentConfig,
    allow_safe_tank: bool,
) -> bool {
    if candidate.locked_out {
        return false;
    }
    if baseline.locked_out {
        return true;
    }
    candidate.headroom_after_rise >= config.strategy_charge_min_headroom
        && (allow_safe_tank
            || candidate.headroom_after_rise
                >= baseline.headroom_after_rise - config.guard_max_headroom_loss)
        && candidate.holes <= baseline.holes + config.guard_max_hole_increase
        && candidate.covered <= baseline.covered + config.guard_max_covered_increase
        && candidate.bumpiness <= baseline.bumpiness + config.guard_max_bumpiness_increase
        && candidate.accessible_holes + config.guard_max_accessible_hole_loss
            >= baseline.accessible_holes
        && (allow_safe_tank || candidate.rise <= baseline.rise)
}

fn charge_diversion_safety_accepts(
    candidate: ActionSafety,
    own: BoardGeometry,
    config: &AgentConfig,
) -> bool {
    !candidate.locked_out
        && candidate.headroom_after_rise >= config.strategy_charge_min_headroom.saturating_add(2)
        && candidate.holes <= own.holes
        && candidate.covered <= own.covered
        && candidate.bumpiness <= own.bumpiness.saturating_add(2)
        && candidate.accessible_holes >= own.accessible_holes
}

fn resource_score(context: &CandidateContext) -> f32 {
    context.future.max_attack as f32 * 1.8
        + context.future.attack_count as f32 * 0.18
        + context.future.combo_continuations as f32 * 0.45
        + context.future.combo_max_attack as f32 * 0.7
        + context.geometry.blocks as f32 * 0.025
        - context.geometry.holes as f32 * 1.5
        - context.geometry.covered as f32 * 0.35
        - context.geometry.max_height.saturating_sub(14) as f32 * 0.8
}

#[allow(clippy::too_many_arguments)]
fn candidate_strategy_event(
    mode: SelectionMode,
    previous_phase: StrategyPhase,
    config: &AgentConfig,
    memory: &StrategyMemory,
    context: &CandidateContext,
    wait_ms: u64,
    projection: TimingProjection,
    _baseline: TimingProjection,
) -> StrategyEvent {
    match mode {
        SelectionMode::Charge => {
            let pieces = memory.charge_pieces.saturating_add(1);
            if pieces >= config.strategy_charge_min_pieces
                && context.future.max_attack >= 4
                && context.future.attack_count > 0
            {
                StrategyEvent::Armed
            } else if previous_phase == StrategyPhase::Neutral {
                StrategyEvent::ChargeEnter
            } else {
                StrategyEvent::ChargeContinue
            }
        }
        SelectionMode::ChargeReleaseIncomingEdge => StrategyEvent::ChargeReleaseIncomingEdge,
        SelectionMode::ChargeReleaseGarbageRiseEdge => StrategyEvent::ChargeReleaseGarbageRiseEdge,
        SelectionMode::Neutral if wait_ms > 0 && projection.fires_simultaneously => {
            StrategyEvent::SurvivalExpertHoldFire
        }
        SelectionMode::Neutral if wait_ms > 0 && projection.fires_after => {
            StrategyEvent::SurvivalExpertHoldFire
        }
        SelectionMode::Neutral => StrategyEvent::SurvivalExpert,
    }
}

fn apply_strategy_event(memory: &mut StrategyMemory, event: StrategyEvent, now_ms: u64) {
    match event {
        StrategyEvent::AttackExpert => {
            memory.expert_mode = ExpertMode::Attack;
            memory.reset_tactic();
        }
        StrategyEvent::AttackExpertHoldFire => {
            memory.expert_mode = ExpertMode::Attack;
            memory.reset_tactic();
            memory.phase = StrategyPhase::HoldFire;
        }
        StrategyEvent::SurvivalExpert => {
            memory.expert_mode = ExpertMode::Survival;
            memory.reset_tactic();
        }
        StrategyEvent::SurvivalExpertHoldFire => {
            memory.expert_mode = ExpertMode::Survival;
            memory.reset_tactic();
            memory.phase = StrategyPhase::HoldFire;
        }
        StrategyEvent::ChargeEnter => {
            memory.expert_mode = ExpertMode::Attack;
            memory.phase = StrategyPhase::Charge;
            memory.charge_started_ms = now_ms;
            memory.charge_pieces = 1;
        }
        StrategyEvent::ChargeContinue => {
            memory.phase = StrategyPhase::Charge;
            memory.charge_pieces = memory.charge_pieces.saturating_add(1);
        }
        StrategyEvent::Armed => {
            memory.expert_mode = ExpertMode::Attack;
            memory.phase = StrategyPhase::Armed;
            memory.charge_pieces = memory.charge_pieces.saturating_add(1);
        }
        StrategyEvent::ChargeReleaseIncomingEdge
        | StrategyEvent::ChargeReleaseGarbageRiseEdge
        | StrategyEvent::ReleaseDodge
        | StrategyEvent::ReleaseCounter
        | StrategyEvent::ReleaseImmediate
        | StrategyEvent::ChargeAbort => memory.reset_tactic(),
        StrategyEvent::RenStart | StrategyEvent::RenContinue => {
            memory.expert_mode = ExpertMode::Attack;
            memory.reset_tactic();
            memory.ren_committed = true;
        }
        StrategyEvent::StackRenEnter
        | StrategyEvent::StackRenBuild
        | StrategyEvent::StackRenFire
        | StrategyEvent::StackRenContinue
        | StrategyEvent::StackRenAbort => {}
        StrategyEvent::None => {}
    }
}

#[derive(Clone)]
struct RenBeamNode {
    board: Board,
    root: ActionKey,
    chain: u32,
    total_attack: u32,
    elapsed_ms: u64,
    pending: Vec<super::IncomingPacket>,
}

#[derive(Clone, Copy, Debug, Default)]
struct RenProjection {
    chain: u32,
    total_attack: u32,
    end_height: u32,
    end_holes: u32,
    end_covered: u32,
}

#[allow(clippy::too_many_arguments)]
fn choose_ren_expert(
    observation: &Observation,
    config: &AgentConfig,
    actions: &[PlacementAction],
    forecast: &OpponentForecast,
    own: BoardGeometry,
    opponent: BoardGeometry,
    cc_floor: &SelectedAction,
    cc_safety: ActionSafety,
    memory: &StrategyMemory,
) -> Option<SelectedAction> {
    let existing_combo = observation.own.board.combo;
    let board_has_resources =
        own.blocks >= 40 && (own.max_height >= 5 || own.holes > 0 || own.bottom_dense_rows >= 2);
    let opponent_vulnerable = opponent.max_height >= config.strategy_attack_enter_height
        || opponent.holes + opponent.covered / 2 >= config.strategy_attack_hole_burden
        || observation.opponent.incoming_total() >= 4;
    // Join a naturally emerging REN one clear earlier. The six-ply projection
    // still has to prove at least eight total attack, so this does not revive
    // the old indiscriminate one-line-clear branch.
    let opportunistic_continuation =
        existing_combo >= 2 && opponent_vulnerable && board_has_resources;
    let continuing = memory.ren_committed || opportunistic_continuation;
    let opponent_attack_1000 = forecast
        .events
        .iter()
        .filter(|event| {
            event.raw_attack > 0 && event.lock_ms <= observation.now_ms.saturating_add(1_000)
        })
        .map(|event| event.raw_attack)
        .sum::<u32>();
    if !continuing && (!board_has_resources || !opponent_vulnerable) {
        return None;
    }
    if !continuing && opponent_attack_1000 > 3 {
        return None;
    }

    let projections = ren_projections(
        observation,
        actions,
        config.strategy_ren_depth,
        config.strategy_ren_beam_width,
    );
    if projections.is_empty() {
        return None;
    }
    let cc_raw = attack_with_pc(
        &cc_floor.action.lock,
        observation.rules.perfect_clear_special_attack,
    );
    let mut best: Option<SelectedAction> = None;
    for action in actions
        .iter()
        .filter(|action| !action.lock.cleared_lines.is_empty() && !action.lock.locked_out)
    {
        let Some(plan) = projections.get(&action.key()).copied() else {
            continue;
        };
        if continuing {
            let minimum_attack = cc_raw.saturating_add(4).max(8);
            if plan.chain < 3 || plan.total_attack < minimum_attack {
                continue;
            }
        } else {
            let minimum_attack = if opponent.max_height >= config.strategy_attack_hard_finish_height
            {
                cc_raw.saturating_add(8).max(14)
            } else {
                cc_raw.saturating_add(6).max(10)
            };
            if plan.chain < config.strategy_ren_start_chain || plan.total_attack < minimum_attack {
                continue;
            }
        }

        let base_lock_ms = lock_ms(observation, action, 0);
        let projection = project_timing(observation, action, forecast, base_lock_ms, base_lock_ms);
        let shell = SelectedAction {
            action: action.clone(),
            wait_ms: 0,
            intent: Intent::ComboContinue,
            score: 0.0,
            policy_override: if action.key() == cc_floor.action.key() {
                PolicyOverride::None
            } else {
                PolicyOverride::Placement
            },
            strategy_event: if continuing {
                StrategyEvent::RenContinue
            } else {
                StrategyEvent::RenStart
            },
            strategy_detail: 0,
        };
        let safety = project_action_safety(observation, &shell);
        if safety.locked_out
            || safety.headroom_after_rise < config.strategy_ren_min_headroom
            || safety.holes > cc_safety.holes.saturating_add(1)
            || safety.covered > cc_safety.covered.saturating_add(2)
        {
            continue;
        }
        let combo = action.lock.combo.unwrap_or(0);
        let score = plan.chain as f32 * 1_000.0
            + plan.total_attack as f32 * 100.0
            + projection.sent as f32 * 30.0
            + combo as f32 * 20.0
            - plan.end_holes as f32 * 12.0
            - plan.end_covered as f32 * 3.0
            - plan.end_height as f32;
        let candidate = SelectedAction { score, ..shell };
        if best
            .as_ref()
            .map(|current| candidate.score > current.score)
            .unwrap_or(true)
        {
            best = Some(candidate);
        }
    }
    best
}

/// Queue-proven REN projection used by the dedicated Stack-REN controller.
/// Returning only aggregate values keeps the v2 planner's internal beam nodes
/// private while allowing v3 to compare build risk with an executable route.
pub(crate) fn best_ren_projection(
    observation: &Observation,
    config: &AgentConfig,
    well: Option<(usize, usize)>,
) -> (u32, u32) {
    best_ren_projection_with_limits(
        observation,
        well,
        config.strategy_ren_depth,
        config.strategy_ren_beam_width,
    )
}

/// A bounded version used by Stack-REN's build lookahead. Keeping the depth
/// and beam explicit lets the builder probe only its strongest placements
/// without multiplying the full production proof-search cost.
pub(crate) fn best_ren_projection_with_limits(
    observation: &Observation,
    well: Option<(usize, usize)>,
    depth: usize,
    beam_width: usize,
) -> (u32, u32) {
    let actions = legal_actions_with_hold(&observation.own.board, observation.own.can_hold)
        .into_iter()
        .filter(|action| well.is_none_or(|well| action_clears_in_well(action, well)))
        .collect::<Vec<_>>();
    ren_projections(observation, &actions, depth, beam_width)
        .values()
        .fold((0, 0), |best, projection| {
            best.max((projection.chain, projection.total_attack))
        })
}

/// Select the first clear of a queue-proven REN with caller-controlled proof
/// thresholds. Stack-REN uses a lower ignition threshold than Strategy v2:
/// the dedicated controller has already paid the risk of building a well, so
/// requiring eight visible attack before the first single clear would make a
/// legitimate four-wide REN impossible to ignite at six-ply depth.
pub(crate) fn choose_queue_proven_ren(
    observation: &Observation,
    config: &AgentConfig,
    fallback: &SelectedAction,
    minimum_chain: u32,
    minimum_attack: u32,
    minimum_headroom: i32,
    well: Option<(usize, usize)>,
) -> Option<SelectedAction> {
    let actions = legal_actions_with_hold(&observation.own.board, observation.own.can_hold);
    let projections = ren_projections(
        observation,
        &actions,
        config.strategy_ren_depth,
        config.strategy_ren_beam_width,
    );
    let fallback_safety = project_action_safety(observation, fallback);
    let forecast = forecast_opponent(observation, config);
    let mut best: Option<SelectedAction> = None;
    for action in actions.iter().filter(|action| {
        !action.lock.cleared_lines.is_empty()
            && !action.lock.locked_out
            && well.is_none_or(|well| action_clears_in_well(action, well))
    }) {
        let Some(plan) = projections.get(&action.key()).copied() else {
            continue;
        };
        if plan.chain < minimum_chain || plan.total_attack < minimum_attack {
            continue;
        }
        let shell = SelectedAction {
            action: action.clone(),
            wait_ms: 0,
            intent: Intent::ComboContinue,
            score: 0.0,
            policy_override: if action.key() == fallback.action.key() {
                PolicyOverride::None
            } else {
                PolicyOverride::Placement
            },
            strategy_event: StrategyEvent::RenContinue,
            strategy_detail: 0,
        };
        let safety = project_action_safety(observation, &shell);
        // `ren_projections` has already advanced FIFO cancellation and every
        // 750ms line-clear boundary. No unsimulated garbage may rise on a
        // retained route, so only the projected stack itself consumes space.
        let horizon_headroom = 20_i32 - plan.end_height as i32;
        if safety.locked_out
            || safety.headroom_after_rise < minimum_headroom
            || horizon_headroom < minimum_headroom
            || safety.holes > fallback_safety.holes.saturating_add(1)
            || safety.covered > fallback_safety.covered.saturating_add(2)
        {
            continue;
        }
        let lock_ms = lock_ms(observation, action, 0);
        let timing = project_timing(observation, action, &forecast, lock_ms, lock_ms);
        let score = plan.chain as f32 * 1_000.0
            + plan.total_attack as f32 * 100.0
            + timing.sent as f32 * 30.0
            - plan.end_holes as f32 * 12.0
            - plan.end_covered as f32 * 3.0
            - plan.end_height as f32;
        let candidate = SelectedAction { score, ..shell };
        if best
            .as_ref()
            .map(|current| candidate.score > current.score)
            .unwrap_or(true)
        {
            best = Some(candidate);
        }
    }
    best
}

fn action_clears_in_well(action: &PlacementAction, (start, width): (usize, usize)) -> bool {
    let end = (start + width).min(10);
    action.placement.cells().iter().any(|&(x, y)| {
        x >= start as i32
            && x < end as i32
            && action.lock.cleared_lines.iter().any(|line| *line == y)
    })
}

fn ren_projections(
    observation: &Observation,
    actions: &[PlacementAction],
    depth: usize,
    beam_width: usize,
) -> HashMap<ActionKey, RenProjection> {
    let depth = depth.max(1);
    let beam_width = beam_width.max(1);
    let mut projections = HashMap::new();
    let mut frontier = Vec::new();
    for action in actions
        .iter()
        .filter(|action| !action.lock.cleared_lines.is_empty() && !action.lock.locked_out)
    {
        let Some((pending, elapsed_ms)) =
            advance_ren_timing(observation, &observation.own.incoming, 0, action)
        else {
            continue;
        };
        let geometry = BoardGeometry::measure(&action.board_after);
        let projection = RenProjection {
            chain: 1,
            total_attack: attack_with_pc(
                &action.lock,
                observation.rules.perfect_clear_special_attack,
            ),
            end_height: geometry.max_height,
            end_holes: geometry.holes,
            end_covered: geometry.covered,
        };
        update_ren_projection(&mut projections, action.key(), projection);
        frontier.push(RenBeamNode {
            board: action.board_after.clone(),
            root: action.key(),
            chain: 1,
            total_attack: projection.total_attack,
            elapsed_ms,
            pending,
        });
    }

    trim_ren_beam(&mut frontier, beam_width);
    for _ in 1..depth {
        let mut next = Vec::new();
        for node in frontier {
            for action in legal_actions(&node.board)
                .into_iter()
                .filter(|action| !action.lock.cleared_lines.is_empty() && !action.lock.locked_out)
            {
                let Some((pending, elapsed_ms)) =
                    advance_ren_timing(observation, &node.pending, node.elapsed_ms, &action)
                else {
                    continue;
                };
                let geometry = BoardGeometry::measure(&action.board_after);
                let chain = node.chain.saturating_add(1);
                let total_attack = node.total_attack.saturating_add(attack_with_pc(
                    &action.lock,
                    observation.rules.perfect_clear_special_attack,
                ));
                update_ren_projection(
                    &mut projections,
                    node.root,
                    RenProjection {
                        chain,
                        total_attack,
                        end_height: geometry.max_height,
                        end_holes: geometry.holes,
                        end_covered: geometry.covered,
                    },
                );
                next.push(RenBeamNode {
                    board: action.board_after,
                    root: node.root,
                    chain,
                    total_attack,
                    elapsed_ms,
                    pending,
                });
            }
        }
        if next.is_empty() {
            break;
        }
        trim_ren_beam(&mut next, beam_width);
        frontier = next;
    }
    projections
}

/// Advance one clear in the exact input/line-clear/garbage-grace contract.
/// Because the beam board does not insert garbage rows, a route remains
/// executable only when every packet that would mature at a line-clear
/// boundary has already been cancelled by attacks earlier in the chain.
fn advance_ren_timing(
    observation: &Observation,
    pending: &[super::IncomingPacket],
    elapsed_ms: u64,
    action: &PlacementAction,
) -> Option<(Vec<super::IncomingPacket>, u64)> {
    let lock_elapsed = elapsed_ms
        .saturating_add(observation.rules.decision_latency_ms)
        .saturating_add(
            observation
                .rules
                .controller_time_ms(action.movements.len(), action.hold),
        );
    let raw_attack = attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
    let remaining = packet_lines_after_cancel(pending, raw_attack);
    let ready_elapsed = lock_elapsed.saturating_add(observation.rules.line_clear_delay_ms);
    if matured_lines(
        &remaining,
        observation.now_ms.saturating_add(ready_elapsed),
        observation.rules.garbage_grace_ms,
    ) > 0
    {
        return None;
    }
    Some((remaining, ready_elapsed))
}

fn update_ren_projection(
    projections: &mut HashMap<ActionKey, RenProjection>,
    root: ActionKey,
    candidate: RenProjection,
) {
    let replace = projections
        .get(&root)
        .map(|current| ren_projection_key(candidate) > ren_projection_key(*current))
        .unwrap_or(true);
    if replace {
        projections.insert(root, candidate);
    }
}

fn ren_projection_key(
    projection: RenProjection,
) -> (u32, u32, std::cmp::Reverse<u32>, std::cmp::Reverse<u32>) {
    (
        projection.chain,
        projection.total_attack,
        std::cmp::Reverse(projection.end_holes),
        std::cmp::Reverse(projection.end_height),
    )
}

fn trim_ren_beam(frontier: &mut Vec<RenBeamNode>, beam_width: usize) {
    frontier.sort_by(|left, right| {
        let left_geometry = BoardGeometry::measure(&left.board);
        let right_geometry = BoardGeometry::measure(&right.board);
        let key = |node: &RenBeamNode, geometry: BoardGeometry| {
            (
                node.chain,
                node.total_attack,
                std::cmp::Reverse(geometry.holes),
                std::cmp::Reverse(geometry.covered),
                std::cmp::Reverse(geometry.max_height),
            )
        };
        key(right, right_geometry).cmp(&key(left, left_geometry))
    });
    frontier.truncate(beam_width);
}

fn quick_rank(observation: &Observation, base_model: &BaseModel, action: &PlacementAction) -> f32 {
    let attack =
        attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack) as f32;
    base_model.score(&base_features(&observation.own.board, action)) + attack * 0.35
}

fn candidate_context(
    observation: &Observation,
    base_model: &BaseModel,
    action: &PlacementAction,
    cache: &mut HashMap<ActionKey, CandidateContext>,
) -> CandidateContext {
    if let Some(context) = cache.get(&action.key()) {
        return context.clone();
    }
    let base_value = base_model.score(&base_features(&observation.own.board, action));
    let base_spike =
        attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack) as f32;
    let context = CandidateContext {
        base_value,
        base_spike,
        geometry: BoardGeometry::measure(&action.board_after),
        future: future_resources(observation, action),
    };
    cache.insert(action.key(), context.clone());
    context
}

fn future_resources(observation: &Observation, action: &PlacementAction) -> FutureResources {
    let next = legal_actions(&action.board_after);
    if next.is_empty() {
        return FutureResources::default();
    }
    let mut attacks = Vec::with_capacity(next.len());
    let mut summary = FutureResources {
        action_count: next.len() as u32,
        ..FutureResources::default()
    };
    for candidate in next {
        let lines = candidate.lock.cleared_lines.len() as u32;
        let attack = attack_with_pc(
            &candidate.lock,
            observation.rules.perfect_clear_special_attack,
        );
        summary.clear_count += (lines > 0) as u32;
        summary.attack_count += (attack > 0) as u32;
        summary.max_attack = summary.max_attack.max(attack);
        attacks.push(attack);
        if lines > 0 && candidate.lock.combo.unwrap_or(0) > action.lock.combo.unwrap_or(0) {
            summary.combo_continuations += 1;
            summary.combo_max_attack = summary.combo_max_attack.max(attack);
        }
    }
    attacks.sort_unstable_by(|left, right| right.cmp(left));
    let count = attacks.len().min(3);
    summary.mean_top_attack = if count == 0 {
        0.0
    } else {
        attacks[..count].iter().sum::<u32>() as f32 / count as f32
    };
    summary
}

#[allow(clippy::too_many_arguments)]
fn strategy_features(
    observation: &Observation,
    forecast: &OpponentForecast,
    own_before: &BoardGeometry,
    opponent: &BoardGeometry,
    action: &PlacementAction,
    wait_ms: u64,
    candidate_lock_ms: u64,
    projection: TimingProjection,
    safety: ActionSafety,
    context: &CandidateContext,
    baseline_context: &CandidateContext,
    baseline_projection: TimingProjection,
    baseline_safety: ActionSafety,
    same_placement: bool,
    memory: &StrategyMemory,
    signals: StateSignals,
    accumulated_resource_gain: f32,
) -> Vec<f32> {
    let raw_attack = attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
    let lines = action.lock.cleared_lines.len() as u32;
    let action_seconds = candidate_lock_ms.saturating_sub(observation.now_ms) as f32 / 1000.0;
    let baseline_action_seconds =
        lock_ms(observation, action, 0).saturating_sub(observation.now_ms) as f32 / 1000.0;
    let safety_margin = 20_i32 - context.geometry.max_height as i32 - projection.rise as i32;
    let dig_delta = own_before
        .bottom_dense_rows
        .saturating_sub(context.geometry.bottom_dense_rows);
    let kill_pressure = projection.pressure as f32
        * (opponent.max_height + observation.opponent.incoming_total().min(20)) as f32
        / 20.0;
    let mut features = vec![
        context.base_value,
        context.base_spike,
        raw_attack as f32,
        projection.sent as f32,
        projection.pressure as f32,
        projection.cancelled as f32,
        lines as f32,
        action_seconds,
        wait_ms as f32 / 1000.0,
        context.geometry.max_height as f32,
        context.geometry.holes as f32,
        context.geometry.covered as f32,
        context.geometry.bumpiness as f32,
        context.geometry.blocks as f32,
        opponent.max_height as f32,
        opponent.holes as f32,
        opponent.covered as f32,
        opponent.blocks as f32,
        observation.own.incoming_total() as f32,
        projection.incoming_at_lock as f32,
        projection.rise as f32,
        safety_margin as f32,
        projection.predicted_attack as f32,
        projection.predicted_outgoing as f32,
        projection.target_lock_ms.saturating_sub(observation.now_ms) as f32 / 1000.0,
        projection.dodge_gain as f32,
        projection.fires_after as u8 as f32,
        projection.fires_before as u8 as f32,
        projection.fires_simultaneously as u8 as f32,
        projection.tank_window as u8 as f32,
        kill_pressure,
        action.lock.combo.unwrap_or(0) as f32,
        action.board_after.b2b_bonus as u8 as f32,
        dig_delta as f32,
        context.geometry.accessible_holes as f32,
        if action.lock.perfect_clear {
            observation.rules.perfect_clear_special_attack as f32
        } else {
            0.0
        },
        if lines > 0 {
            observation.rules.line_clear_delay_ms as f32 / 1000.0
        } else {
            0.0
        },
        observation.opponent.incoming_total() as f32,
        (lines > 0) as u8 as f32,
        context.future.max_attack as f32,
        projection.dodge_gain as f32,
        (projection.pressure + context.future.max_attack) as f32,
        observation.now_ms.saturating_sub(memory.charge_started_ms) as f32 / 1000.0,
        (signals.safe_nonfire_options > 0 && signals.charge_safety_score >= 0.0) as u8 as f32,
    ];
    debug_assert_eq!(features.len(), FEATURE_NAMES.len());

    let remaining = packet_lines_after_cancel(&observation.own.incoming, raw_attack);
    let due = |packets: &[super::IncomingPacket], horizon_ms: u64| {
        matured_lines(
            packets,
            candidate_lock_ms.saturating_add(horizon_ms),
            observation.rules.garbage_grace_ms,
        )
    };
    let due_500_before = due(&observation.own.incoming, 500);
    let due_500_after = due(&remaining, 500);
    let first_rise_seconds = observation
        .own
        .incoming
        .iter()
        .map(|packet| {
            packet
                .arrival_ms
                .saturating_add(observation.rules.garbage_grace_ms)
                .saturating_add(1)
                .saturating_sub(observation.now_ms)
        })
        .min()
        .unwrap_or(4_000) as f32
        / 1000.0;
    let forecast_sum = |horizon_ms: u64, outgoing: bool| {
        forecast
            .events
            .iter()
            .filter(|event| event.lock_ms <= observation.now_ms.saturating_add(horizon_ms))
            .map(|event| {
                if outgoing {
                    event.outgoing_attack
                } else {
                    event.raw_attack
                }
            })
            .sum::<u32>()
    };
    let forecast_max_burst = forecast
        .events
        .iter()
        .map(|event| event.outgoing_attack)
        .max()
        .unwrap_or(0);
    let first_attack_seconds = forecast
        .events
        .iter()
        .find(|event| event.raw_attack > 0)
        .map(|event| event.lock_ms.saturating_sub(observation.now_ms) as f32 / 1000.0)
        .unwrap_or(4.0);
    let timing_slack_seconds = forecast
        .events
        .iter()
        .filter(|event| event.raw_attack > 0)
        .map(|event| event.lock_ms.abs_diff(candidate_lock_ms))
        .min()
        .unwrap_or(4_000) as f32
        / 1000.0;
    let own_headroom = 20_i32 - context.geometry.max_height as i32 - projection.rise as i32;
    let opponent_headroom =
        20_i32 - opponent.max_height as i32 - projection.pressure.min(40) as i32;
    let extras = vec![
        same_placement as u8 as f32,
        (!same_placement) as u8 as f32,
        context.base_value - baseline_context.base_value,
        context.base_spike - baseline_context.base_spike,
        (safety.headroom_after_rise - baseline_safety.headroom_after_rise) as f32,
        context.geometry.holes as f32 - baseline_context.geometry.holes as f32,
        context.geometry.covered as f32 - baseline_context.geometry.covered as f32,
        context.geometry.bumpiness as f32 - baseline_context.geometry.bumpiness as f32,
        context.geometry.accessible_holes as f32
            - baseline_context.geometry.accessible_holes as f32,
        projection.rise as f32 - baseline_projection.rise as f32,
        projection.cancelled as f32 - baseline_projection.cancelled as f32,
        projection.sent as f32 - baseline_projection.sent as f32,
        projection.pressure as f32 - baseline_projection.pressure as f32,
        action_seconds - baseline_action_seconds,
        own_headroom as f32,
        opponent_headroom as f32,
        (-opponent_headroom).max(0) as f32,
        matured_lines(
            &observation.own.incoming,
            candidate_lock_ms,
            observation.rules.garbage_grace_ms,
        ) as f32,
        due(&remaining, 250) as f32,
        due_500_after as f32,
        due(&remaining, 1_000) as f32,
        first_rise_seconds,
        due_500_before.saturating_sub(due_500_after) as f32,
        due_500_after as f32,
        forecast.confidence,
        forecast_sum(500, false) as f32,
        forecast_sum(1_000, false) as f32,
        forecast_sum(2_000, false) as f32,
        forecast_sum(500, true) as f32,
        forecast_sum(1_000, true) as f32,
        forecast_sum(2_000, true) as f32,
        forecast_max_burst as f32,
        first_attack_seconds,
        forecast.events.len() as f32,
        context.future.action_count as f32,
        context.future.clear_count as f32,
        context.future.attack_count as f32,
        context.future.max_attack as f32,
        context.future.mean_top_attack,
        context.future.combo_continuations as f32,
        context.future.combo_max_attack as f32,
        action.lock.combo.unwrap_or(0) as f32,
        context.geometry.bottom_dense_rows as f32,
        context.geometry.holes as f32 + context.geometry.covered as f32 * 0.5,
        raw_attack as f32 / action_seconds.max(0.05),
        projection.pressure as f32 / action_seconds.max(0.05),
        (projection.target_lock_ms as i64 - candidate_lock_ms as i64) as f32 / 1000.0,
        if projection.fires_simultaneously {
            projection.pressure as f32
        } else {
            0.0
        },
        context.geometry.blocks as f32 - opponent.blocks as f32,
        opponent.holes as f32 + opponent.covered as f32 * 0.5,
        context.geometry.holes as f32 + context.geometry.covered as f32 * 0.5,
        (-own_headroom).max(0) as f32 + due_500_after as f32,
        wait_ms as f32 / observation.rules.garbage_grace_ms.max(1) as f32,
        timing_slack_seconds,
        baseline_projection
            .sent
            .saturating_add(baseline_projection.cancelled) as f32,
        baseline_projection.sent as f32,
    ];
    debug_assert_eq!(extras.len(), STRATEGY_EXTRA_FEATURE_NAMES.len());
    features.extend(extras);
    let state = vec![
        (memory.phase == StrategyPhase::Charge) as u8 as f32,
        (memory.phase == StrategyPhase::Armed) as u8 as f32,
        memory.charge_pieces as f32,
        if memory.phase == StrategyPhase::Neutral {
            0.0
        } else {
            observation.now_ms.saturating_sub(memory.charge_started_ms) as f32 / 1000.0
        },
        signals.immediate_attack_options as f32,
        signals.safe_nonfire_options as f32,
        signals.max_immediate_attack as f32,
        signals.incoming_edge.lines() as f32,
        signals.incoming_edge.is_edge() as u8 as f32,
        if signals.opponent_attack_eta_ms == u64::MAX {
            4.0
        } else {
            signals.opponent_attack_eta_ms as f32 / 1000.0
        },
        signals.charge_safety_score,
        accumulated_resource_gain,
        piece_distance(&action.board_after, Piece::I),
        piece_distance(&action.board_after, Piece::T),
        matches!(action.board_after.hold_piece, Some(Piece::I | Piece::T)) as u8 as f32,
        context.future.combo_continuations as f32
            + context.future.combo_max_attack as f32
            + action.lock.combo.unwrap_or(0) as f32,
    ];
    debug_assert_eq!(state.len(), STRATEGY_STATE_FEATURE_NAMES.len());
    features.extend(state);
    features
}

fn piece_distance(board: &libtetris::Board, target: Piece) -> f32 {
    board
        .next_queue()
        .position(|piece| piece == target)
        .map(|index| index as f32)
        .unwrap_or(8.0)
}

fn strategy_wait_candidates(
    observation: &Observation,
    action: &PlacementAction,
    forecast: &OpponentForecast,
    base_lock_ms: u64,
    maximum_wait_ms: u64,
) -> Vec<u64> {
    let raw = attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
    let mut waits = vec![0];
    if raw == 0 || maximum_wait_ms == 0 {
        return waits;
    }
    let quantum = observation.rules.input_interval_ms.max(1);
    for event in forecast.events.iter().filter(|event| event.raw_attack > 0) {
        for offset in [-2_i64, -1, 0, 1, 2] {
            let target = event.lock_ms as i128 + offset as i128 * quantum as i128;
            if target <= base_lock_ms as i128 {
                continue;
            }
            let raw_wait = (target as u64).saturating_sub(base_lock_ms);
            for quantized in [
                raw_wait / quantum * quantum,
                raw_wait.div_ceil(quantum) * quantum,
            ] {
                if quantized > 0 && quantized <= maximum_wait_ms {
                    waits.push(quantized);
                }
            }
        }
    }
    waits.sort_unstable();
    waits.dedup();
    waits
}

fn safe_same_placement_wait(
    candidate: ActionSafety,
    baseline: ActionSafety,
    projection: TimingProjection,
    baseline_projection: TimingProjection,
) -> bool {
    !candidate.locked_out
        && candidate.headroom_after_rise >= baseline.headroom_after_rise
        && candidate.rise <= baseline.rise
        && projection.rise <= baseline_projection.rise
}

fn timing_improves(candidate: TimingProjection, baseline: TimingProjection) -> bool {
    // Strategy v2 is the offensive model. A hold is accepted only when the
    // same raw attack is predicted to escape more cancellation and therefore
    // send strictly more pressure. Defensive cancellation belongs to Guard.
    candidate.sent > baseline.sent && candidate.cancelled < baseline.cancelled
}

fn classify_override(
    same_placement: bool,
    wait_ms: u64,
    projection: TimingProjection,
    baseline: TimingProjection,
) -> PolicyOverride {
    if !same_placement {
        return if wait_ms > 0 {
            PolicyOverride::PlacementAndWait
        } else {
            PolicyOverride::Placement
        };
    }
    if wait_ms == 0 {
        return PolicyOverride::None;
    }
    if projection.fires_simultaneously && projection.pressure > baseline.pressure {
        PolicyOverride::SamePlacementDodge
    } else if projection.fires_after && projection.cancelled > baseline.cancelled {
        PolicyOverride::SamePlacementCounter
    } else {
        PolicyOverride::SamePlacementWait
    }
}

fn classify_strategy_intent(
    observation: &Observation,
    action: &PlacementAction,
    wait_ms: u64,
    projection: TimingProjection,
    baseline: TimingProjection,
    own_before: BoardGeometry,
) -> Intent {
    let geometry = BoardGeometry::measure(&action.board_after);
    let headroom = 20_i32 - geometry.max_height as i32 - projection.rise as i32;
    if headroom <= 2 || action.lock.locked_out {
        Intent::Survival
    } else if wait_ms > 0 && (projection.dodge_gain > 0 || projection.pressure > baseline.pressure)
    {
        Intent::CancellationDodge
    } else if projection.cancelled > baseline.cancelled
        || (projection.cancelled > 0 && projection.sent == 0)
    {
        Intent::Counter
    } else if action.lock.combo.unwrap_or(0) >= 2 {
        Intent::ComboContinue
    } else if projection.sent > 0 {
        Intent::SpikeNow
    } else if own_before.bottom_dense_rows > geometry.bottom_dense_rows
        || !action.lock.cleared_lines.is_empty()
    {
        Intent::Dig
    } else if observation.own.incoming_total() > 0 {
        Intent::Survival
    } else {
        Intent::Stack
    }
}

fn lock_ms(observation: &Observation, action: &PlacementAction, wait_ms: u64) -> u64 {
    observation
        .now_ms
        .saturating_add(observation.rules.decision_latency_ms)
        .saturating_add(wait_ms)
        .saturating_add(
            observation
                .rules
                .controller_time_ms(action.movements.len(), action.hold),
        )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent::{AgentController, AgentKind, PhaseView, PlayerView};
    use crate::rules::Rules;
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

    #[test]
    fn v2_always_has_equal_node_cc_fallback() {
        let board = queued_board();
        let observation = Observation {
            now_ms: 0,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 0,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 0,
                average_piece_ms: 350.0,
            },
        };
        let mut config = AgentConfig::default();
        config.cold_clear_nodes = 128;
        config.tempo_model_override = Some(TempoModel::strategy_v2_bootstrap());
        let mut controller = AgentController::new(AgentKind::Kasane, config).unwrap();
        assert!(controller.choose(&observation).is_some());
    }

    #[test]
    fn active_charge_diverts_only_for_already_queued_fire() {
        assert!(should_divert_charge(
            SelectionMode::Charge,
            StrategyPhase::Armed,
            5,
            4,
        ));
        assert!(!should_divert_charge(
            SelectionMode::Charge,
            StrategyPhase::Charge,
            5,
            0,
        ));
        assert!(!should_divert_charge(
            SelectionMode::Charge,
            StrategyPhase::Neutral,
            5,
            4,
        ));
    }

    #[test]
    fn wait_lattice_contains_counter_and_dodge_neighbours() {
        let board = queued_board();
        let action = legal_actions(&board)[0].clone();
        let observation = Observation {
            now_ms: 0,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 0,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 0,
                average_piece_ms: 350.0,
            },
        };
        let base = lock_ms(&observation, &action, 0);
        let mut forecast = OpponentForecast::default();
        forecast.events.push(super::super::ForecastEvent {
            action: action.clone(),
            lock_ms: base + 300,
            raw_attack: 4,
            outgoing_attack: 4,
        });
        let waits = strategy_wait_candidates(&observation, &action, &forecast, base, 750);
        if attack_with_pc(&action.lock, 0) > 0 {
            assert!(waits.contains(&300));
            assert!(waits.contains(&350));
        } else {
            assert_eq!(waits, vec![0]);
        }
    }

    #[test]
    fn stack_ren_root_must_clear_through_the_committed_well() {
        let mut board = Board::new();
        let mut field = [[false; 10]; 40];
        for x in 0..10 {
            field[0][x] = !(3..7).contains(&x);
        }
        board.set_field(field);
        for piece in [Piece::I, Piece::O, Piece::T, Piece::S, Piece::Z] {
            board.add_next_piece(piece);
        }
        let action = legal_actions(&board)
            .into_iter()
            .find(|action| !action.lock.cleared_lines.is_empty())
            .expect("horizontal I must ignite the four-wide well");
        assert!(action_clears_in_well(&action, (3, 4)));
        assert!(!action_clears_in_well(&action, (0, 2)));
    }

    #[test]
    fn queue_proven_ren_rejects_garbage_that_rises_during_line_clear() {
        let mut board = Board::new();
        let mut field = [[false; 10]; 40];
        for x in 0..10 {
            field[0][x] = !(3..7).contains(&x);
        }
        board.set_field(field);
        for piece in [Piece::I, Piece::O, Piece::T, Piece::S, Piece::Z] {
            board.add_next_piece(piece);
        }
        let player = PlayerView {
            board: board.clone(),
            can_hold: true,
            incoming: Vec::new(),
            phase: PhaseView::Ready,
            pieces: 0,
            average_piece_ms: 350.0,
        };
        let mut safe = Observation {
            now_ms: 100,
            rules: Rules::pinned(),
            own: player.clone(),
            opponent: player,
        };
        let actions = legal_actions(&board);
        assert!(!ren_projections(&safe, &actions, 2, 20).is_empty());

        safe.own.incoming.push(super::super::IncomingPacket {
            lines: 1,
            arrival_ms: 0,
        });
        assert!(ren_projections(&safe, &actions, 2, 20).is_empty());
    }

    #[test]
    fn charge_gate_accepts_equal_geometry_when_resource_improves() {
        let safety = ActionSafety {
            locked_out: false,
            headroom_after_rise: 10,
            holes: 0,
            covered: 0,
            bumpiness: 4,
            accessible_holes: 0,
            cancelled: 0,
            rise: 0,
        };
        let config = AgentConfig::default();
        assert!(charge_safety_gate_accepts(safety, safety, &config, false));

        let baseline = CandidateContext {
            base_value: 0.0,
            base_spike: 0.0,
            geometry: BoardGeometry::default(),
            future: FutureResources::default(),
        };
        let candidate = CandidateContext {
            future: FutureResources {
                attack_count: 2,
                max_attack: 4,
                ..FutureResources::default()
            },
            ..baseline.clone()
        };
        assert!(charge_resource_improves(&candidate, &baseline));
    }

    #[test]
    fn charge_gate_rejects_headroom_or_hole_regression() {
        let baseline = ActionSafety {
            locked_out: false,
            headroom_after_rise: 10,
            holes: 0,
            covered: 0,
            bumpiness: 4,
            accessible_holes: 1,
            cancelled: 0,
            rise: 0,
        };
        let config = AgentConfig::default();
        assert!(!charge_safety_gate_accepts(
            ActionSafety {
                headroom_after_rise: 9,
                ..baseline
            },
            baseline,
            &config,
            false,
        ));
        assert!(!charge_safety_gate_accepts(
            ActionSafety {
                holes: 1,
                ..baseline
            },
            baseline,
            &config,
            false,
        ));
    }

    #[test]
    fn diversion_gate_compares_against_the_pre_clear_board() {
        let own = BoardGeometry::default();
        let baseline = ActionSafety {
            locked_out: false,
            headroom_after_rise: 12,
            holes: 0,
            covered: 0,
            bumpiness: 2,
            accessible_holes: 0,
            cancelled: 0,
            rise: 4,
        };
        let config = AgentConfig::default();
        assert!(charge_diversion_safety_accepts(baseline, own, &config));
        assert!(!charge_diversion_safety_accepts(
            ActionSafety {
                holes: 1,
                ..baseline
            },
            own,
            &config,
        ));
        assert!(!charge_diversion_safety_accepts(
            ActionSafety {
                headroom_after_rise: 11,
                ..baseline
            },
            own,
            &config,
        ));
    }

    #[test]
    fn queued_incoming_edge_is_observed_once() {
        let board = queued_board();
        let mut observation = Observation {
            now_ms: 0,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 0,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 0,
                average_piece_ms: 350.0,
            },
        };
        let mut memory = StrategyMemory::default();
        assert!(!memory.observe_incoming(&observation).is_edge());
        observation.now_ms = 100;
        observation.own.incoming.push(super::super::IncomingPacket {
            lines: 4,
            arrival_ms: 100,
        });
        let first = memory.observe_incoming(&observation);
        assert_eq!(first.queued_lines, 4);
        assert_eq!(first.risen_lines, 0);
        assert!(!memory.observe_incoming(&observation).is_edge());
    }

    #[test]
    fn garbage_rise_edge_is_detected_from_expected_post_action_board() {
        let mut board = queued_board();
        let expected = BoardGeometry::measure(&board).blocks;
        assert!(!board.add_garbage(0));
        let observation = Observation {
            now_ms: 1_001,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 1,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 1,
                average_piece_ms: 350.0,
            },
        };
        let mut memory = StrategyMemory {
            expected_post_action_blocks: Some(expected),
            ..StrategyMemory::default()
        };
        let edge = memory.observe_incoming(&observation);
        assert_eq!(edge.queued_lines, 0);
        assert_eq!(edge.risen_lines, 1);
    }

    #[test]
    fn armed_charge_never_releases_from_forecast_alone() {
        let board = queued_board();
        let observation = Observation {
            now_ms: 500,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 4,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 4,
                average_piece_ms: 350.0,
            },
        };
        let mut memory = StrategyMemory {
            phase: StrategyPhase::Armed,
            charge_started_ms: 100,
            charge_pieces: 3,
            ..StrategyMemory::default()
        };
        let signals = StateSignals {
            opponent_attack_eta_ms: 0,
            immediate_attack_options: 2,
            safe_nonfire_options: 2,
            ..StateSignals::default()
        };
        assert_eq!(
            selection_mode(
                &observation,
                &AgentConfig::default(),
                &mut memory,
                signals,
                BoardGeometry::measure(&observation.own.board),
                BoardGeometry::measure(&observation.opponent.board),
            ),
            SelectionMode::Charge
        );
    }

    #[test]
    fn armed_charge_releases_immediately_on_garbage_rise_edge() {
        let board = queued_board();
        let observation = Observation {
            now_ms: 1_500,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 5,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 5,
                average_piece_ms: 350.0,
            },
        };
        let mut memory = StrategyMemory {
            phase: StrategyPhase::Armed,
            charge_started_ms: 100,
            charge_pieces: 4,
            ..StrategyMemory::default()
        };
        let signals = StateSignals {
            incoming_edge: IncomingObservation {
                queued_lines: 0,
                risen_lines: 2,
            },
            immediate_attack_options: 1,
            safe_nonfire_options: 1,
            ..StateSignals::default()
        };
        assert_eq!(
            selection_mode(
                &observation,
                &AgentConfig::default(),
                &mut memory,
                signals,
                BoardGeometry::measure(&observation.own.board),
                BoardGeometry::measure(&observation.opponent.board),
            ),
            SelectionMode::ChargeReleaseGarbageRiseEdge
        );
    }

    #[test]
    fn rise_release_edge_persists_until_the_next_fireable_piece() {
        let board = queued_board();
        let mut observation = Observation {
            now_ms: 1_500,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 5,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 5,
                average_piece_ms: 350.0,
            },
        };
        let mut memory = StrategyMemory {
            phase: StrategyPhase::Armed,
            charge_started_ms: 100,
            charge_pieces: 4,
            ..StrategyMemory::default()
        };
        let own = BoardGeometry::measure(&observation.own.board);
        let opponent = BoardGeometry::measure(&observation.opponent.board);
        let no_fire_on_edge = StateSignals {
            incoming_edge: IncomingObservation {
                queued_lines: 0,
                risen_lines: 2,
            },
            immediate_attack_options: 0,
            safe_nonfire_options: 1,
            ..StateSignals::default()
        };
        assert_eq!(
            selection_mode(
                &observation,
                &AgentConfig::default(),
                &mut memory,
                no_fire_on_edge,
                own,
                opponent,
            ),
            SelectionMode::Charge
        );
        assert!(memory.release_pending_until_ms > observation.now_ms);

        observation.now_ms += 100;
        let fireable_next_piece = StateSignals {
            immediate_attack_options: 1,
            safe_nonfire_options: 1,
            ..StateSignals::default()
        };
        assert_eq!(
            selection_mode(
                &observation,
                &AgentConfig::default(),
                &mut memory,
                fireable_next_piece,
                own,
                opponent,
            ),
            SelectionMode::ChargeReleaseGarbageRiseEdge
        );
    }

    #[test]
    fn active_finishing_expert_falls_back_to_survival_instead_of_starting_charge() {
        let board = queued_board();
        let observation = Observation {
            now_ms: 500,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 4,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 4,
                average_piece_ms: 350.0,
            },
        };
        let mut memory = StrategyMemory {
            expert_mode: ExpertMode::Attack,
            ..StrategyMemory::default()
        };
        let signals = StateSignals {
            opponent_attack_eta_ms: u64::MAX,
            safe_nonfire_options: 3,
            ..StateSignals::default()
        };
        let opponent = BoardGeometry {
            max_height: AgentConfig::default().strategy_attack_enter_height,
            ..BoardGeometry::default()
        };
        assert_eq!(
            selection_mode(
                &observation,
                &AgentConfig::default(),
                &mut memory,
                signals,
                BoardGeometry::measure(&observation.own.board),
                opponent,
            ),
            SelectionMode::Neutral
        );
    }

    #[test]
    fn absent_forecast_attack_does_not_start_a_charge() {
        let board = queued_board();
        let observation = Observation {
            now_ms: 500,
            rules: Rules::pinned(),
            own: PlayerView {
                board: board.clone(),
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 4,
                average_piece_ms: 350.0,
            },
            opponent: PlayerView {
                board,
                can_hold: true,
                incoming: Vec::new(),
                phase: PhaseView::Ready,
                pieces: 4,
                average_piece_ms: 350.0,
            },
        };
        let mut memory = StrategyMemory::default();
        let signals = StateSignals {
            opponent_attack_eta_ms: u64::MAX,
            safe_nonfire_options: 3,
            ..StateSignals::default()
        };
        let opponent = BoardGeometry {
            max_height: AgentConfig::default().strategy_attack_enter_height,
            ..BoardGeometry::default()
        };
        assert_eq!(
            selection_mode(
                &observation,
                &AgentConfig::default(),
                &mut memory,
                signals,
                BoardGeometry::measure(&observation.own.board),
                opponent,
            ),
            SelectionMode::Neutral
        );
    }

    #[test]
    fn hold_fire_and_charge_release_are_distinct_memory_events() {
        let mut memory = StrategyMemory::default();
        apply_strategy_event(&mut memory, StrategyEvent::AttackExpertHoldFire, 100);
        assert_eq!(memory.phase, StrategyPhase::HoldFire);
        assert_eq!(memory.expert_mode, ExpertMode::Attack);
        memory.finish_previous_hold_fire();
        assert_eq!(memory.phase, StrategyPhase::Neutral);

        memory.phase = StrategyPhase::Armed;
        memory.charge_pieces = 4;
        apply_strategy_event(
            &mut memory,
            StrategyEvent::ChargeReleaseGarbageRiseEdge,
            200,
        );
        assert_eq!(memory.phase, StrategyPhase::Neutral);
    }
}
