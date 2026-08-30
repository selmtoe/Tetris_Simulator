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
        let width = FEATURE_NAMES.len();
        if self.feature_names != FEATURE_NAMES {
            bail!("tempo model feature schema does not match this KASANE build");
        }
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
        debug_assert_eq!(raw.len(), FEATURE_NAMES.len());
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
