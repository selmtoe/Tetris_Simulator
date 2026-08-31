use super::{
    forecast_opponent, matured_lines, packet_lines_after_cancel, AgentConfig, IncomingPacket,
    Intent, Observation, OpponentForecast, SelectedAction,
};
use crate::base::{analyze_base_with_hold, BaseModel};
use crate::model::{BoardGeometry, TempoModel, FEATURE_NAMES};
#[cfg(test)]
use crate::search::pc0_attack;
use crate::search::{attack_with_pc, legal_actions, legal_actions_with_hold, PlacementAction};
use std::cmp::Ordering;

#[derive(Clone, Copy, Debug)]
pub(crate) struct TimingProjection {
    pub(crate) sent: u32,
    pub(crate) pressure: u32,
    pub(crate) cancelled: u32,
    pub(crate) incoming_at_lock: u32,
    pub(crate) rise: u32,
    pub(crate) dodge_gain: i32,
    pub(crate) fires_after: bool,
    pub(crate) fires_before: bool,
    pub(crate) fires_simultaneously: bool,
    pub(crate) tank_window: bool,
    pub(crate) predicted_attack: u32,
    pub(crate) predicted_outgoing: u32,
    pub(crate) target_lock_ms: u64,
}

#[derive(Clone, Copy, Debug, Default)]
struct ChargeProjection {
    attack: u32,
    dodge_gain: i32,
    pressure: u32,
    seconds: f32,
    feasible: bool,
}

pub fn choose_base_only(
    observation: &Observation,
    config: &AgentConfig,
    base_model: &BaseModel,
) -> Option<SelectedAction> {
    let analysis = analyze_base_with_hold(
        &observation.own.board,
        base_model,
        config.base_depth,
        config.base_beam_width,
        observation.own.incoming_total(),
        observation.own.can_hold,
    );
    let action = analysis.chosen?;
    let score = analysis
        .root_scores
        .get(&action.key())
        .map(|(value, spike)| value + spike * 0.12)
        .unwrap_or_default();
    Some(SelectedAction {
        action,
        wait_ms: 0,
        intent: Intent::Stack,
        score,
        policy_override: super::PolicyOverride::None,
        strategy_event: super::StrategyEvent::None,
        strategy_detail: 0,
    })
}

pub fn choose_kasane(
    observation: &Observation,
    config: &AgentConfig,
    base_model: &BaseModel,
    model: &TempoModel,
) -> Option<SelectedAction> {
    let actions = legal_actions_with_hold(&observation.own.board, observation.own.can_hold);
    if actions.is_empty() {
        return None;
    }
    let own_base = analyze_base_with_hold(
        &observation.own.board,
        base_model,
        config.base_depth,
        config.base_beam_width,
        observation.own.incoming_total(),
        observation.own.can_hold,
    );
    let forecast = forecast_opponent(observation, config);
    let own_before = BoardGeometry::measure(&observation.own.board);
    let opponent_geometry = BoardGeometry::measure(&observation.opponent.board);
    let base_choice_key = own_base.chosen.as_ref().map(PlacementAction::key);

    let mut best: Option<SelectedAction> = None;
    for action in actions {
        // Only actions evaluated by KASANE-Base's own beam are eligible for a
        // tactical override.
        if !own_base.root_scores.contains_key(&action.key()) {
            continue;
        }
        let base_lock_ms = observation.now_ms
            + observation.rules.decision_latency_ms
            + observation
                .rules
                .controller_time_ms(action.movements.len(), action.hold);
        for wait_ms in wait_candidates(
            observation,
            &action,
            &forecast,
            base_lock_ms,
            config.maximum_wait_ms,
            config.enable_tank,
        ) {
            if config.strict_base_policy && wait_ms == 0 && base_choice_key != Some(action.key()) {
                continue;
            }
            let lock_ms = base_lock_ms + wait_ms;
            let projection = project_timing(observation, &action, &forecast, lock_ms, base_lock_ms);
            let charge = if config.enable_charge {
                project_charge(observation, &action, &forecast, lock_ms, config)
            } else {
                ChargeProjection::default()
            };
            let after = BoardGeometry::measure(&action.board_after);
            let (base_value, base_spike) = own_base
                .root_scores
                .get(&action.key())
                .copied()
                .unwrap_or((-2_000.0, 0.0));
            let raw_attack =
                attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
            let lines = action.lock.cleared_lines.len() as u32;
            let is_clear = lines > 0;
            let action_ms = lock_ms.saturating_sub(observation.now_ms);
            let safety_margin = 20_i32 - after.max_height as i32 - projection.rise as i32;
            let finish_height = opponent_geometry.max_height + projection.pressure;
            if wait_ms > 0
                && (!projection.fires_simultaneously
                    || projection.dodge_gain <= 0
                    || wait_ms > config.dodge_wait_cap_ms
                    || finish_height < config.dodge_finish_height
                    || safety_margin < config.dodge_safety_margin)
            {
                continue;
            }
            let dig_delta = own_before
                .bottom_dense_rows
                .saturating_sub(after.bottom_dense_rows);
            let kill_pressure = projection.pressure as f32
                * (opponent_geometry.max_height + observation.opponent.incoming_total().min(20))
                    as f32
                / 20.0;

            let features = vec![
                base_value,
                base_spike,
                raw_attack as f32,
                projection.sent as f32,
                projection.pressure as f32,
                projection.cancelled as f32,
                lines as f32,
                action_ms as f32 / 1000.0,
                wait_ms as f32 / 1000.0,
                after.max_height as f32,
                after.holes as f32,
                after.covered as f32,
                after.bumpiness as f32,
                after.blocks as f32,
                opponent_geometry.max_height as f32,
                opponent_geometry.holes as f32,
                opponent_geometry.covered as f32,
                opponent_geometry.blocks as f32,
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
                after.accessible_holes as f32,
                if action.lock.perfect_clear {
                    observation.rules.perfect_clear_special_attack as f32
                } else {
                    0.0
                },
                if is_clear {
                    observation.rules.line_clear_delay_ms as f32 / 1000.0
                } else {
                    0.0
                },
                observation.opponent.incoming_total() as f32,
                is_clear as u8 as f32,
                charge.attack as f32,
                charge.dodge_gain as f32,
                charge.pressure as f32,
                charge.seconds,
                charge.feasible as u8 as f32,
            ];
            debug_assert_eq!(features.len(), FEATURE_NAMES.len());
            let mut score = model.score(&features);
            if wait_ms == 0 && base_choice_key == Some(action.key()) {
                // Conservative policy improvement: KASANE-Base is the safe
                // baseline. A tactical branch must earn this margin through
                // packet survival or immediate safety.
                score += config.base_guard_margin;
            }
            if action.lock.locked_out || safety_margin < -3 {
                score -= 1_000_000.0;
            } else if safety_margin < 0 {
                score -= 100.0 * (-safety_margin) as f32;
            }
            // This is a safety constraint, not a learned preference: never
            // tank enough mature garbage to cross the visible ceiling.
            if projection.rise > 0 && projection.rise + after.max_height >= 20 {
                score -= 100_000.0;
            }
            let intent = classify_intent(
                &action,
                projection,
                charge,
                dig_delta,
                safety_margin,
                wait_ms,
            );
            let candidate = SelectedAction {
                action: action.clone(),
                wait_ms,
                intent,
                score,
                policy_override: super::PolicyOverride::None,
                strategy_event: super::StrategyEvent::None,
                strategy_detail: 0,
            };
            if best
                .as_ref()
                .map(|current| {
                    candidate
                        .score
                        .partial_cmp(&current.score)
                        .unwrap_or(Ordering::Less)
                        == Ordering::Greater
                })
                .unwrap_or(true)
            {
                best = Some(candidate);
            }
        }
    }
    best
}

fn project_charge(
    observation: &Observation,
    first: &PlacementAction,
    forecast: &OpponentForecast,
    first_lock_ms: u64,
    config: &AgentConfig,
) -> ChargeProjection {
    if attack_with_pc(&first.lock, observation.rules.perfect_clear_special_attack) > 0
        || first.lock.locked_out
    {
        return ChargeProjection::default();
    }
    let next_start_ms = first_lock_ms
        + if first.lock.cleared_lines.is_empty() {
            0
        } else {
            observation.rules.line_clear_delay_ms
        };
    // If garbage rises between the two planned placements, the predicted
    // second board is invalid. Survival logic handles that state instead.
    if matured_lines(
        &observation.own.incoming,
        next_start_ms,
        observation.rules.garbage_grace_ms,
    ) > 0
    {
        return ChargeProjection::default();
    }

    let mut best = ChargeProjection::default();
    for second in legal_actions(&first.board_after) {
        let attack = attack_with_pc(&second.lock, observation.rules.perfect_clear_special_attack);
        if attack == 0 || second.lock.locked_out {
            continue;
        }
        let base_second_lock = next_start_ms
            + observation.rules.decision_latency_ms
            + observation
                .rules
                .controller_time_ms(second.movements.len(), second.hold);
        for event in &forecast.events {
            if event.raw_attack == 0 || event.lock_ms < base_second_lock {
                continue;
            }
            let wait = event.lock_ms - base_second_lock;
            if wait > config.maximum_wait_ms.min(config.dodge_wait_cap_ms)
                || wait % observation.rules.input_interval_ms != 0
            {
                continue;
            }
            let timing = project_timing(
                observation,
                &second,
                forecast,
                event.lock_ms,
                base_second_lock,
            );
            let safety = 20_i32
                - *second
                    .board_after
                    .column_heights()
                    .iter()
                    .max()
                    .unwrap_or(&40)
                - timing.rise as i32;
            let opponent_height = BoardGeometry::measure(&observation.opponent.board).max_height;
            if safety < config.dodge_safety_margin
                || timing.dodge_gain <= 0
                || !timing.fires_simultaneously
                || opponent_height + timing.pressure < config.dodge_finish_height
            {
                continue;
            }
            let seconds = event.lock_ms.saturating_sub(observation.now_ms) as f32 / 1000.0;
            let candidate = ChargeProjection {
                attack,
                dodge_gain: timing.dodge_gain,
                pressure: timing.pressure,
                seconds,
                feasible: true,
            };
            let candidate_score = candidate.dodge_gain as f32 * 3.0
                + candidate.pressure as f32
                + candidate.attack as f32 * 0.25
                - seconds * 0.2;
            let best_score =
                best.dodge_gain as f32 * 3.0 + best.pressure as f32 + best.attack as f32 * 0.25
                    - best.seconds * 0.2;
            if !best.feasible || candidate_score > best_score {
                best = candidate;
            }
        }
    }
    best
}

fn wait_candidates(
    observation: &Observation,
    action: &PlacementAction,
    forecast: &OpponentForecast,
    base_lock_ms: u64,
    maximum_wait_ms: u64,
    enable_tank: bool,
) -> Vec<u64> {
    let quantum = observation.rules.input_interval_ms;
    let mut waits = vec![0];
    let mut add_target_near = |target_lock_ms: u64| {
        if target_lock_ms <= base_lock_ms {
            return;
        }
        let raw = target_lock_ms - base_lock_ms;
        let floor = raw / quantum * quantum;
        let ceil = raw.div_ceil(quantum) * quantum;
        for quantized in [floor, ceil] {
            if quantized > 0 && quantized <= maximum_wait_ms {
                waits.push(quantized);
            }
        }
    };

    let raw_attack = attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
    if forecast.action.is_some() && raw_attack > 0 {
        // The exact same lock timestamp is the clean cancellation-dodge:
        // both sides cancel only their pre-existing queues, then both new
        // packets are delivered. Neighbouring 50 ms lattice points are kept
        // too so the model can reject an uncertain forecast safely.
        for event in &forecast.events {
            if event.raw_attack > 0 {
                add_target_near(event.lock_ms);
            }
        }
    }
    if enable_tank && action.lock.cleared_lines.is_empty() {
        for packet in &observation.own.incoming {
            let target = packet
                .arrival_ms
                .saturating_add(observation.rules.garbage_grace_ms)
                .saturating_add(1);
            if target.saturating_sub(base_lock_ms) <= 500 {
                add_target_near(target);
            }
        }
    }
    waits.sort_unstable();
    waits.dedup();
    waits
}

pub(crate) fn project_timing(
    observation: &Observation,
    action: &PlacementAction,
    forecast: &OpponentForecast,
    lock_ms: u64,
    base_lock_ms: u64,
) -> TimingProjection {
    let target_lock_ms = forecast
        .events
        .iter()
        .filter(|event| event.raw_attack > 0)
        .min_by_key(|event| event.lock_ms.abs_diff(lock_ms))
        .map(|event| event.lock_ms);
    let chosen = projection_at(observation, action, forecast, lock_ms, target_lock_ms);
    let immediate = projection_at(observation, action, forecast, base_lock_ms, target_lock_ms);
    TimingProjection {
        dodge_gain: chosen.pressure as i32 - immediate.pressure as i32,
        ..chosen
    }
}

fn projection_at(
    observation: &Observation,
    action: &PlacementAction,
    forecast: &OpponentForecast,
    lock_ms: u64,
    target_lock_ms: Option<u64>,
) -> TimingProjection {
    let raw = attack_with_pc(&action.lock, observation.rules.perfect_clear_special_attack);
    let mut packets = observation.own.incoming.clone();
    let mut opponent_packets = observation.opponent.incoming.clone();
    let mut opponent_risen = 0_u32;
    if let super::PhaseView::LineClear { ends_ms } = observation.opponent.phase {
        // The engine raises mature garbage before scheduling the next piece.
        opponent_risen += drain_matured_packets(
            &mut opponent_packets,
            ends_ms,
            observation.rules.garbage_grace_ms,
        );
    }
    let horizon_ms = target_lock_ms.unwrap_or(lock_ms).max(lock_ms);
    let mut fired = false;
    let mut sent = 0;
    let mut cancelled = 0;
    let mut incoming_at_lock = observation.own.incoming_total();
    let mut target_attack = 0;
    let mut target_outgoing = 0;

    for event in forecast
        .events
        .iter()
        .filter(|event| event.lock_ms <= horizon_ms)
    {
        if !fired && lock_ms < event.lock_ms {
            incoming_at_lock = packets.iter().map(|packet| packet.lines).sum();
            cancelled = raw.min(incoming_at_lock);
            sent = raw - cancelled;
            packets = packet_lines_after_cancel(&packets, raw);
            if sent > 0 {
                opponent_packets.push(IncomingPacket {
                    lines: sent,
                    arrival_ms: lock_ms,
                });
            }
            fired = true;
        }

        let is_target = Some(event.lock_ms) == target_lock_ms;
        if !fired && lock_ms == event.lock_ms {
            // Simultaneous lock: each attack sees only packets that existed
            // before this timestamp. New leftovers are delivered afterwards.
            incoming_at_lock = packets.iter().map(|packet| packet.lines).sum();
            let opponent_pending = packet_total(&opponent_packets);
            let opponent_outgoing = event.raw_attack.saturating_sub(opponent_pending);
            opponent_packets = packet_lines_after_cancel(&opponent_packets, event.raw_attack);
            cancelled = raw.min(incoming_at_lock);
            sent = raw - cancelled;
            packets = packet_lines_after_cancel(&packets, raw);
            if sent > 0 {
                opponent_packets.push(IncomingPacket {
                    lines: sent,
                    arrival_ms: lock_ms,
                });
            }
            if opponent_outgoing > 0 {
                packets.push(IncomingPacket {
                    lines: opponent_outgoing,
                    arrival_ms: event.lock_ms,
                });
            }
            if is_target {
                target_attack = event.raw_attack;
                target_outgoing = opponent_outgoing;
            }
            let opponent_rise_ms = event.lock_ms
                + if event.action.lock.cleared_lines.is_empty() {
                    0
                } else {
                    observation.rules.line_clear_delay_ms
                };
            opponent_risen += drain_matured_packets(
                &mut opponent_packets,
                opponent_rise_ms,
                observation.rules.garbage_grace_ms,
            );
            fired = true;
            continue;
        }

        let opponent_pending = packet_total(&opponent_packets);
        let opponent_outgoing = event.raw_attack.saturating_sub(opponent_pending);
        opponent_packets = packet_lines_after_cancel(&opponent_packets, event.raw_attack);
        if opponent_outgoing > 0 {
            packets.push(IncomingPacket {
                lines: opponent_outgoing,
                arrival_ms: event.lock_ms,
            });
        }
        if is_target {
            target_attack = event.raw_attack;
            target_outgoing = opponent_outgoing;
        }
        let opponent_rise_ms = event.lock_ms
            + if event.action.lock.cleared_lines.is_empty() {
                0
            } else {
                observation.rules.line_clear_delay_ms
            };
        opponent_risen += drain_matured_packets(
            &mut opponent_packets,
            opponent_rise_ms,
            observation.rules.garbage_grace_ms,
        );
    }

    if !fired {
        incoming_at_lock = packets.iter().map(|packet| packet.lines).sum();
        cancelled = raw.min(incoming_at_lock);
        sent = raw - cancelled;
        packets = packet_lines_after_cancel(&packets, raw);
        if sent > 0 {
            opponent_packets.push(IncomingPacket {
                lines: sent,
                arrival_ms: lock_ms,
            });
        }
    }
    if target_lock_ms.is_none() {
        target_attack = forecast.raw_attack;
        target_outgoing = forecast.outgoing_attack;
    }

    let rise_time = if action.lock.cleared_lines.is_empty() {
        lock_ms
    } else {
        lock_ms.saturating_add(observation.rules.line_clear_delay_ms)
    };
    let rise = matured_lines(&packets, rise_time, observation.rules.garbage_grace_ms);
    let safety_after = 20_i32
        - *action
            .board_after
            .column_heights()
            .iter()
            .max()
            .unwrap_or(&40)
        - rise as i32;
    let target = target_lock_ms.unwrap_or(forecast.lock_ms);
    let before_reply = forecast.action.is_some() && lock_ms < target;
    let after_reply = forecast.action.is_some() && lock_ms > target;
    let simultaneous = forecast.action.is_some() && lock_ms == target;
    let tank_window =
        raw == 0 && action.lock.cleared_lines.is_empty() && rise > 0 && safety_after >= 3;

    TimingProjection {
        sent,
        pressure: packet_total(&opponent_packets).saturating_add(opponent_risen),
        cancelled,
        incoming_at_lock,
        rise,
        dodge_gain: 0,
        fires_after: after_reply && raw > 0,
        fires_before: before_reply && raw > 0,
        fires_simultaneously: simultaneous && raw > 0,
        tank_window,
        predicted_attack: target_attack,
        predicted_outgoing: target_outgoing,
        target_lock_ms: target,
    }
}

fn packet_total(packets: &[IncomingPacket]) -> u32 {
    packets.iter().map(|packet| packet.lines).sum()
}

fn drain_matured_packets(packets: &mut Vec<IncomingPacket>, at_ms: u64, grace_ms: u64) -> u32 {
    let mut risen = 0_u32;
    packets.retain(|packet| {
        let matured = at_ms.saturating_sub(packet.arrival_ms) > grace_ms;
        if matured {
            risen = risen.saturating_add(packet.lines);
        }
        !matured
    });
    risen
}

fn classify_intent(
    action: &PlacementAction,
    timing: TimingProjection,
    charge: ChargeProjection,
    dig_delta: u32,
    safety_margin: i32,
    wait_ms: u64,
) -> Intent {
    let raw = timing.sent + timing.cancelled;
    if safety_margin <= 2 {
        return Intent::Survival;
    }
    if timing.tank_window && wait_ms > 0 {
        return Intent::TankThenFire;
    }
    if raw == 0 && charge.feasible {
        return Intent::Charge;
    }
    if raw > 0 && timing.dodge_gain > 0 {
        return Intent::CancellationDodge;
    }
    if raw > 0 && timing.cancelled > 0 && timing.sent == 0 {
        return Intent::Counter;
    }
    if action.lock.combo.unwrap_or(0) >= 2 {
        return Intent::ComboContinue;
    }
    if timing.sent > 0 {
        return Intent::SpikeNow;
    }
    if dig_delta > 0 || !action.lock.cleared_lines.is_empty() {
        return Intent::Dig;
    }
    Intent::Stack
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent::{ForecastEvent, PhaseView, PlayerView};
    use crate::rules::Rules;
    use libtetris::{Board, Piece};

    fn tetris_well_board() -> Board {
        let mut board = Board::new();
        let mut field = [[false; 10]; 40];
        for row in field.iter_mut().take(4) {
            for (x, cell) in row.iter_mut().enumerate() {
                *cell = x != 4;
            }
        }
        board.set_field(field);
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
    fn simultaneous_fire_dodges_cancellation_but_early_fire_does_not() {
        let board = tetris_well_board();
        let action = legal_actions(&board)
            .into_iter()
            .find(|candidate| pc0_attack(&candidate.lock) == 4)
            .expect("vertical I must complete the four-row well");
        let player = PlayerView {
            board: board.clone(),
            can_hold: true,
            incoming: Vec::new(),
            phase: PhaseView::Ready,
            pieces: 0,
            average_piece_ms: 300.0,
        };
        let observation = Observation {
            now_ms: 0,
            rules: Rules::pinned(),
            own: player.clone(),
            opponent: player,
        };
        let forecast = OpponentForecast {
            action: Some(action.clone()),
            lock_ms: 1_000,
            raw_attack: 4,
            outgoing_attack: 4,
            confidence: 1.0,
            events: vec![ForecastEvent {
                action: action.clone(),
                lock_ms: 1_000,
                raw_attack: 4,
                outgoing_attack: 4,
            }],
        };

        let early = projection_at(&observation, &action, &forecast, 500, Some(1_000));
        let simultaneous = projection_at(&observation, &action, &forecast, 1_000, Some(1_000));

        assert_eq!(early.pressure, 0, "early fire is cancelled by the reply");
        assert!(!early.fires_simultaneously);
        assert_eq!(simultaneous.pressure, 4, "same-timestamp fire survives");
        assert_eq!(simultaneous.sent, 4);
        assert_eq!(simultaneous.predicted_outgoing, 4);
        assert!(simultaneous.fires_simultaneously);
    }

    #[test]
    fn opponent_line_clear_rise_cannot_cancel_its_next_attack() {
        let board = tetris_well_board();
        let action = legal_actions(&board)
            .into_iter()
            .find(|candidate| pc0_attack(&candidate.lock) == 4)
            .expect("vertical I must complete the four-row well");
        let own = PlayerView {
            board: board.clone(),
            can_hold: true,
            incoming: Vec::new(),
            phase: PhaseView::Ready,
            pieces: 0,
            average_piece_ms: 300.0,
        };
        let mut opponent = own.clone();
        opponent.incoming = vec![IncomingPacket {
            lines: 4,
            arrival_ms: 0,
        }];
        opponent.phase = PhaseView::LineClear { ends_ms: 1_001 };
        let observation = Observation {
            now_ms: 500,
            rules: Rules::pinned(),
            own,
            opponent,
        };
        let forecast = OpponentForecast {
            action: Some(action.clone()),
            lock_ms: 1_200,
            raw_attack: 4,
            outgoing_attack: 0,
            confidence: 1.0,
            events: vec![ForecastEvent {
                action: action.clone(),
                lock_ms: 1_200,
                raw_attack: 4,
                outgoing_attack: 0,
            }],
        };

        let projection = projection_at(&observation, &action, &forecast, 1_200, Some(1_200));
        assert_eq!(projection.predicted_outgoing, 4);
    }

    #[test]
    fn opponent_pending_matures_between_forecast_clears() {
        let board = tetris_well_board();
        let action = legal_actions(&board)
            .into_iter()
            .find(|candidate| pc0_attack(&candidate.lock) == 4)
            .expect("vertical I must complete the four-row well");
        let own = PlayerView {
            board: board.clone(),
            can_hold: true,
            incoming: Vec::new(),
            phase: PhaseView::Ready,
            pieces: 0,
            average_piece_ms: 300.0,
        };
        let mut opponent = own.clone();
        opponent.incoming = vec![IncomingPacket {
            lines: 6,
            arrival_ms: 0,
        }];
        opponent.phase = PhaseView::Moving { started_ms: 0 };
        let observation = Observation {
            now_ms: 0,
            rules: Rules::pinned(),
            own,
            opponent,
        };
        let forecast = OpponentForecast {
            action: Some(action.clone()),
            lock_ms: 300,
            raw_attack: 2,
            outgoing_attack: 0,
            confidence: 1.0,
            events: vec![
                ForecastEvent {
                    action: action.clone(),
                    lock_ms: 300,
                    raw_attack: 2,
                    outgoing_attack: 0,
                },
                ForecastEvent {
                    action: action.clone(),
                    lock_ms: 1_200,
                    raw_attack: 4,
                    outgoing_attack: 0,
                },
            ],
        };

        let projection = projection_at(&observation, &action, &forecast, 1_200, Some(1_200));
        assert_eq!(projection.predicted_outgoing, 4);
    }
}
