use super::{AgentConfig, IncomingPacket, Observation, SelectedAction};
use crate::base::BaseModel;
use crate::model::{BoardGeometry, TempoModel};
use crate::search::attack_with_pc;

#[derive(Copy, Clone, Debug)]
pub(crate) struct ActionSafety {
    pub locked_out: bool,
    pub headroom_after_rise: i32,
    pub holes: u32,
    pub covered: u32,
    pub bumpiness: u32,
    pub accessible_holes: u32,
    pub cancelled: u32,
    pub rise: u32,
}

pub(crate) fn choose_guard(
    observation: &Observation,
    config: &AgentConfig,
    base_model: &BaseModel,
    tempo_model: &TempoModel,
    fallback: Option<SelectedAction>,
) -> Option<SelectedAction> {
    let tactical = super::kasane::choose_kasane(observation, config, base_model, tempo_model);
    match (tactical, fallback) {
        (None, fallback) => fallback,
        (Some(tactical), None) => Some(tactical),
        (Some(tactical), Some(fallback)) => {
            if safety_gate_accepts(observation, &tactical, &fallback, config) {
                Some(tactical)
            } else {
                Some(fallback)
            }
        }
    }
}

pub(crate) fn safety_gate_accepts(
    observation: &Observation,
    tactical: &SelectedAction,
    fallback: &SelectedAction,
    config: &AgentConfig,
) -> bool {
    if tactical.action.key() == fallback.action.key() && tactical.wait_ms == fallback.wait_ms {
        return true;
    }
    let tactical_safety = project_action_safety(observation, tactical);
    let fallback_safety = project_action_safety(observation, fallback);
    safety_metrics_accept(
        &tactical_safety,
        &fallback_safety,
        config.guard_max_headroom_loss,
        config.guard_max_hole_increase,
        config.guard_max_covered_increase,
        config.guard_max_bumpiness_increase,
        config.guard_max_accessible_hole_loss,
        config.guard_minimum_cancel_gain,
    )
}

fn safety_metrics_accept(
    tactical: &ActionSafety,
    fallback: &ActionSafety,
    max_headroom_loss: i32,
    max_hole_increase: u32,
    max_covered_increase: u32,
    max_bumpiness_increase: u32,
    max_accessible_hole_loss: u32,
    minimum_cancel_gain: u32,
) -> bool {
    if tactical.locked_out {
        return false;
    }
    if fallback.locked_out {
        return true;
    }
    let geometry_safe = tactical.headroom_after_rise
        >= fallback.headroom_after_rise - max_headroom_loss
        && tactical.holes <= fallback.holes + max_hole_increase
        && tactical.covered <= fallback.covered + max_covered_increase
        && tactical.bumpiness <= fallback.bumpiness + max_bumpiness_increase
        && tactical.accessible_holes + max_accessible_hole_loss >= fallback.accessible_holes;
    if !geometry_safe {
        return false;
    }
    tactical.headroom_after_rise > fallback.headroom_after_rise
        || tactical.holes < fallback.holes
        || tactical.covered < fallback.covered
        || tactical.bumpiness < fallback.bumpiness
        || tactical.accessible_holes > fallback.accessible_holes
        || tactical.rise < fallback.rise
        || tactical.cancelled >= fallback.cancelled.saturating_add(minimum_cancel_gain)
}

pub(crate) fn project_action_safety(
    observation: &Observation,
    selected: &SelectedAction,
) -> ActionSafety {
    let raw_attack = attack_with_pc(
        &selected.action.lock,
        observation.rules.perfect_clear_special_attack,
    );
    let incoming_total = observation.own.incoming_total();
    let cancelled = raw_attack.min(incoming_total);
    let mut attack_left = raw_attack;
    let mut remaining = Vec::with_capacity(observation.own.incoming.len());
    for packet in &observation.own.incoming {
        let used = attack_left.min(packet.lines);
        attack_left -= used;
        if packet.lines > used {
            remaining.push(IncomingPacket {
                lines: packet.lines - used,
                arrival_ms: packet.arrival_ms,
            });
        }
    }
    let lock_ms = observation.now_ms
        + observation.rules.decision_latency_ms
        + selected.wait_ms
        + observation
            .rules
            .controller_time_ms(selected.action.movements.len(), selected.action.hold);
    let rise_ms = lock_ms
        + if selected.action.lock.cleared_lines.is_empty() {
            0
        } else {
            observation.rules.line_clear_delay_ms
        };
    let rise = remaining
        .iter()
        .filter(|packet| {
            rise_ms.saturating_sub(packet.arrival_ms) > observation.rules.garbage_grace_ms
        })
        .map(|packet| packet.lines)
        .sum::<u32>();
    let geometry = BoardGeometry::measure(&selected.action.board_after);
    ActionSafety {
        locked_out: selected.action.lock.locked_out,
        headroom_after_rise: 20 - geometry.max_height as i32 - rise as i32,
        holes: geometry.holes,
        covered: geometry.covered,
        bumpiness: geometry.bumpiness,
        accessible_holes: geometry.accessible_holes,
        cancelled,
        rise,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn baseline() -> ActionSafety {
        ActionSafety {
            locked_out: false,
            headroom_after_rise: 5,
            holes: 2,
            covered: 3,
            bumpiness: 5,
            accessible_holes: 2,
            cancelled: 1,
            rise: 2,
        }
    }

    #[test]
    fn gate_rejects_geometry_regression_for_extra_cancellation() {
        let fallback = baseline();
        let tactical = ActionSafety {
            headroom_after_rise: 4,
            cancelled: 4,
            ..fallback
        };
        assert!(!safety_metrics_accept(
            &tactical, &fallback, 0, 0, 0, 2, 0, 1
        ));
    }

    #[test]
    fn gate_accepts_safe_cancellation_gain() {
        let fallback = baseline();
        let tactical = ActionSafety {
            cancelled: 2,
            ..fallback
        };
        assert!(safety_metrics_accept(
            &tactical, &fallback, 0, 0, 0, 2, 0, 1
        ));
    }
}
