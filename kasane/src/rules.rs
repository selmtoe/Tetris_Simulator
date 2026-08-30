use anyhow::{bail, Result};
use serde::{Deserialize, Serialize};

/// Immutable gameplay contract for the first KASANE model.
///
/// All gameplay timestamps are integral milliseconds.  Thinking is assigned a
/// fixed, equal latency so faster training hardware cannot alter game speed.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(default)]
pub struct Rules {
    pub schema: String,
    pub input_interval_ms: u64,
    pub line_clear_delay_ms: u64,
    pub garbage_grace_ms: u64,
    pub decision_latency_ms: u64,
    pub preview_count: usize,
    pub garbage_randomness: f64,
    pub perfect_clear_special_attack: u32,
    pub spawn_delay_ms: u64,
}

impl Default for Rules {
    fn default() -> Self {
        Self::pinned()
    }
}

impl Rules {
    pub fn pinned() -> Self {
        Self {
            schema: "kasane-rules/ppt-50-750-1000-pc0-v1".to_owned(),
            input_interval_ms: 50,
            line_clear_delay_ms: 750,
            garbage_grace_ms: 1000,
            decision_latency_ms: 180,
            preview_count: 8,
            garbage_randomness: 0.30,
            perfect_clear_special_attack: 0,
            spawn_delay_ms: 0,
        }
    }

    pub fn validate(&self) -> Result<()> {
        if self.input_interval_ms != 50 {
            bail!("KASANE v1 requires input_interval_ms=50");
        }
        if self.line_clear_delay_ms != 750 {
            bail!("KASANE v1 requires line_clear_delay_ms=750");
        }
        if self.garbage_grace_ms != 1000 {
            bail!("KASANE v1 requires garbage_grace_ms=1000");
        }
        if self.preview_count == 0 {
            bail!("preview_count must be positive");
        }
        if !(0.0..=1.0).contains(&self.garbage_randomness) {
            bail!("garbage_randomness must be in [0, 1]");
        }
        if self.perfect_clear_special_attack != 0 {
            bail!("the PC0 benchmark requires perfect_clear_special_attack=0");
        }
        Ok(())
    }

    pub fn controller_time_ms(&self, movement_count: usize, hold: bool) -> u64 {
        // One input for HOLD when used and one final hard-drop input.  Every
        // movement emitted by libtetris is an additional controller input.
        let inputs = movement_count as u64 + u64::from(hold) + 1;
        inputs * self.input_interval_ms
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pinned_contract_cannot_drift() {
        let rules = Rules::pinned();
        rules.validate().unwrap();
        assert_eq!(rules.input_interval_ms, 50);
        assert_eq!(rules.line_clear_delay_ms, 750);
        assert_eq!(rules.garbage_grace_ms, 1000);
        assert_eq!(rules.controller_time_ms(3, true), 250);
    }
}
