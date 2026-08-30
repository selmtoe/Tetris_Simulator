use crate::model::{BoardGeometry, DenseLayer};
use crate::search::{legal_actions, pc0_attack, ActionKey, PlacementAction};
use anyhow::{bail, Context, Result};
use libtetris::{Board, Piece, PlacementKind, RotationState, TspinStatus};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::Path;

pub const BASE_CORE_FEATURE_NAMES: [&str; 37] = [
    "max_height",
    "sum_height",
    "holes",
    "holes_squared",
    "covered",
    "bumpiness",
    "bumpiness_squared",
    "row_transitions",
    "blocks",
    "top_half",
    "top_quarter",
    "well_depth",
    "max_well_depth",
    "well_center_bias",
    "back_to_back_state",
    "bottom_dense_rows",
    "accessible_holes",
    "height_delta",
    "holes_delta",
    "dig_delta",
    "attack_pc0",
    "cleared_lines",
    "combo",
    "combo_attack",
    "b2b_clear",
    "clear1",
    "clear2",
    "clear3",
    "clear4",
    "tspin1",
    "tspin2",
    "tspin3",
    "used_hold",
    "controller_inputs",
    "wasted_t",
    "perfect_clear",
    "locked_out",
];

pub const BASE_CELL_ROWS: usize = 20;
pub const BASE_PREVIEW_PIECES: usize = 8;

pub fn base_feature_names() -> Vec<String> {
    let mut names: Vec<String> = BASE_CORE_FEATURE_NAMES
        .iter()
        .map(|name| (*name).to_owned())
        .collect();
    names.extend((0..10).map(|x| format!("column_height_{x}")));
    names.extend((0..10).map(|x| format!("column_holes_{x}")));
    names.extend((0..9).map(|x| format!("skyline_delta_{x}")));
    names.extend((0..BASE_CELL_ROWS).map(|y| format!("row_fill_{y}")));
    for y in 0..BASE_CELL_ROWS {
        names.extend((0..10).map(move |x| format!("cell_{y}_{x}")));
    }
    names.extend(piece_names("placed_piece"));
    names.push("hold_none".to_owned());
    names.extend(piece_names("hold"));
    for preview in 0..BASE_PREVIEW_PIECES {
        names.extend(piece_names(&format!("next_{preview}")));
    }
    names.push("placement_x".to_owned());
    names.push("placement_y".to_owned());
    names.extend([
        "rotation_north".to_owned(),
        "rotation_east".to_owned(),
        "rotation_south".to_owned(),
        "rotation_west".to_owned(),
        "tspin_none".to_owned(),
        "tspin_mini".to_owned(),
        "tspin_full".to_owned(),
    ]);
    names
}

fn piece_names(prefix: &str) -> impl Iterator<Item = String> + '_ {
    ["i", "o", "t", "l", "j", "s", "z"]
        .into_iter()
        .map(move |piece| format!("{prefix}_{piece}"))
}

/// Independently owned stacking policy/value model. Cold Clear is permitted
/// to teach this model offline, but this type and its inference path contain
/// no Cold Clear evaluator or DAG.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct BaseModel {
    pub schema: String,
    #[serde(default)]
    pub search_mode: BaseSearchMode,
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

#[derive(Copy, Clone, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BaseSearchMode {
    /// A distilled policy score already contains its teacher's lookahead.
    #[default]
    DirectPolicy,
    /// Static board value is evaluated at the leaf while transition rewards
    /// are accumulated once along the beam path.
    ValueReward,
}

impl Default for BaseModel {
    fn default() -> Self {
        let names = base_feature_names();
        let mut scale = vec![1.0; names.len()];
        let mut weights = vec![0.0; names.len()];
        let mut set = |name: &str, divisor: f32, weight: f32| {
            let index = names
                .iter()
                .position(|candidate| *candidate == name)
                .unwrap();
            scale[index] = divisor;
            weights[index] = weight;
        };

        // Bootstrap prior, intentionally compact and inspectable. These are
        // not copied Cold Clear weights; training replaces/refines them.
        // Cold Clear's feature families informed the starting ratios, but the
        // evaluator, timing terms and final optimized weights are KASANE's.
        // Coefficients below include the pinned 750 ms clear-delay cost and
        // deliberately value reproducible Tetris/T-spin pressure highly.
        set("max_height", 10.0, -3.90);
        set("sum_height", 50.0, -1.00);
        set("holes", 5.0, -8.65);
        set("holes_squared", 25.0, -0.75);
        set("covered", 10.0, -1.70);
        set("bumpiness", 10.0, -2.40);
        set("bumpiness_squared", 100.0, -7.00);
        set("row_transitions", 20.0, -1.00);
        set("blocks", 100.0, 0.00);
        set("top_half", 10.0, -1.50);
        set("top_quarter", 5.0, -2.55);
        set("well_depth", 4.0, 2.28);
        set("max_well_depth", 8.0, 1.36);
        set("well_center_bias", 1.0, -0.03);
        set("back_to_back_state", 1.0, 0.52);
        set("bottom_dense_rows", 8.0, -0.08);
        set("accessible_holes", 5.0, 0.40);
        set("height_delta", 4.0, -0.20);
        set("holes_delta", 3.0, -1.20);
        set("dig_delta", 3.0, 0.90);
        set("attack_pc0", 4.0, 0.40);
        set("cleared_lines", 4.0, 0.00);
        set("combo", 4.0, 0.00);
        set("combo_attack", 3.0, 4.50);
        set("b2b_clear", 1.0, 1.04);
        set("clear1", 1.0, -2.63);
        set("clear2", 1.0, -2.20);
        set("clear3", 1.0, -1.78);
        set("clear4", 1.0, 2.70);
        set("tspin1", 1.0, 0.01);
        set("tspin2", 1.0, 2.90);
        set("tspin3", 1.0, 4.82);
        set("used_hold", 1.0, -0.01);
        set("controller_inputs", 5.0, -0.15);
        set("wasted_t", 1.0, -1.52);
        set("perfect_clear", 1.0, 0.0); // PC0, explicitly immutable in spirit.
        set("locked_out", 1.0, -1000.0);
        for x in 0..10 {
            set(&format!("column_height_{x}"), 10.0, 0.0);
            set(&format!("column_holes_{x}"), 5.0, 0.0);
        }
        for x in 0..9 {
            set(&format!("skyline_delta_{x}"), 5.0, 0.0);
        }
        for y in 0..BASE_CELL_ROWS {
            set(&format!("row_fill_{y}"), 10.0, 0.0);
            for x in 0..10 {
                set(&format!("cell_{y}_{x}"), 1.0, 0.0);
            }
        }
        set("placement_x", 10.0, 0.0);
        set("placement_y", 20.0, 0.0);

        Self {
            schema: "kasane-base-model/v2-cell".to_owned(),
            search_mode: BaseSearchMode::ValueReward,
            feature_names: names,
            input_mean: vec![0.0; scale.len()],
            input_scale: scale,
            linear_weights: weights,
            linear_bias: 0.0,
            hidden_layers: Vec::new(),
            neural_output_weights: Vec::new(),
            neural_output_bias: 0.0,
        }
    }
}

impl BaseModel {
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
        fs::write(path, serde_json::to_vec_pretty(self)?)?;
        Ok(())
    }

    pub fn validate(&self) -> Result<()> {
        let names = base_feature_names();
        let width = names.len();
        if self.feature_names != names {
            bail!("base model feature schema does not match this KASANE build");
        }
        if self.input_mean.len() != width
            || self.input_scale.len() != width
            || self.linear_weights.len() != width
        {
            bail!("base model input dimensions are invalid");
        }
        if self
            .input_scale
            .iter()
            .any(|scale| !scale.is_finite() || *scale <= 0.0)
        {
            bail!("base model scales must be finite and positive");
        }
        let mut input_width = width;
        for layer in &self.hidden_layers {
            if layer.weights.len() != layer.bias.len()
                || layer.weights.iter().any(|row| row.len() != input_width)
            {
                bail!("base model hidden layer dimensions are invalid");
            }
            input_width = layer.bias.len();
        }
        if !self.hidden_layers.is_empty() && self.neural_output_weights.len() != input_width {
            bail!("base model output dimensions are invalid");
        }
        Ok(())
    }

    pub fn score(&self, raw: &[f32]) -> f32 {
        let (state, reward) = self.score_components(raw);
        state + reward
    }

    fn score_components(&self, raw: &[f32]) -> (f32, f32) {
        debug_assert_eq!(raw.len(), self.feature_names.len());
        let mut activations: Vec<f32> = raw
            .iter()
            .zip(&self.input_mean)
            .zip(&self.input_scale)
            .map(|((&value, &mean), &scale)| (value - mean) / scale)
            .collect();
        if self.search_mode == BaseSearchMode::DirectPolicy {
            let linear = self.linear_bias + dot(&self.linear_weights, &activations);
            if self.hidden_layers.is_empty() {
                return (linear, 0.0);
            }
            for layer in &self.hidden_layers {
                activations = layer
                    .weights
                    .iter()
                    .zip(&layer.bias)
                    .map(|(weights, bias)| (dot(weights, &activations) + bias).max(0.0))
                    .collect();
            }
            return (
                linear + self.neural_output_bias + dot(&self.neural_output_weights, &activations),
                0.0,
            );
        }

        let mut state = self.linear_bias;
        let mut reward = 0.0;
        for (index, (&weight, &activation)) in
            self.linear_weights.iter().zip(&activations).enumerate()
        {
            if (17..=36).contains(&index) {
                reward += weight * activation;
            } else {
                state += weight * activation;
            }
        }
        if self.hidden_layers.is_empty() {
            return (state, reward);
        }
        for layer in &self.hidden_layers {
            activations = layer
                .weights
                .iter()
                .zip(&layer.bias)
                .map(|(weights, bias)| (dot(weights, &activations) + bias).max(0.0))
                .collect();
        }
        state += self.neural_output_bias + dot(&self.neural_output_weights, &activations);
        (state, reward)
    }
}

#[derive(Clone, Debug)]
pub struct BaseAnalysis {
    pub chosen: Option<PlacementAction>,
    pub root_scores: HashMap<ActionKey, (f32, f32)>,
    pub expanded: usize,
}

#[derive(Clone)]
struct BeamNode {
    board: Board,
    root: ActionKey,
    value: f32,
    accumulated_reward: f32,
    spike: f32,
}

pub fn analyze_base(
    board: &Board,
    model: &BaseModel,
    depth: usize,
    beam_width: usize,
    incoming: u32,
) -> BaseAnalysis {
    let root_actions = legal_actions(board);
    if root_actions.is_empty() {
        return BaseAnalysis {
            chosen: None,
            root_scores: HashMap::new(),
            expanded: 0,
        };
    }
    let depth = depth.max(1);
    let beam_width = beam_width.max(1);
    let mut root_scores = HashMap::new();
    let mut frontier = Vec::with_capacity(root_actions.len());
    for action in &root_actions {
        let (state, reward) = model.score_components(&base_features(board, action));
        let value = state + reward;
        let spike = pc0_attack(&action.lock) as f32;
        root_scores.insert(action.key(), (value, spike));
        if !action.lock.locked_out {
            frontier.push(BeamNode {
                board: action.board_after.clone(),
                root: action.key(),
                value,
                accumulated_reward: reward,
                spike,
            });
        }
    }
    let mut expanded = root_actions.len();
    trim_beam(&mut frontier, beam_width);
    for _level in 1..depth {
        let mut next = Vec::with_capacity(frontier.len() * 32);
        let mut level_scores = HashMap::new();
        for node in frontier {
            for action in legal_actions(&node.board) {
                expanded += 1;
                if action.lock.locked_out {
                    continue;
                }
                let attack = pc0_attack(&action.lock) as f32;
                let spike = if action.lock.cleared_lines.is_empty() {
                    0.0
                } else {
                    node.spike + attack
                };
                let (state, reward) = model.score_components(&base_features(&node.board, &action));
                let accumulated_reward = node.accumulated_reward + reward;
                let value = state + accumulated_reward;
                let candidate_score = (value, spike);
                let entry = level_scores.entry(node.root).or_insert(candidate_score);
                // Keep value and spike from the same continuation. Combining
                // their independent maxima fabricates a path that cannot be
                // played and can change the selected root action.
                if score_pair(candidate_score) > score_pair(*entry) {
                    *entry = candidate_score;
                }
                next.push(BeamNode {
                    board: action.board_after,
                    root: node.root,
                    value,
                    accumulated_reward,
                    spike,
                });
            }
        }
        if next.is_empty() {
            break;
        }
        // Every root score now describes the same completed search horizon.
        // A shallow root estimate must never compete with a deeper leaf.
        root_scores = level_scores;
        trim_beam(&mut next, beam_width);
        frontier = next;
    }

    let chosen = root_actions
        .iter()
        .filter(|action| root_scores.contains_key(&action.key()))
        .filter(|action| {
            let remaining = incoming.saturating_sub(pc0_attack(&action.lock)) as i32;
            action.board_after.column_heights()[3..6]
                .iter()
                .all(|height| height + remaining <= 20)
        })
        .max_by(|left, right| {
            let left_score = root_scores.get(&left.key()).copied().unwrap_or_default();
            let right_score = root_scores.get(&right.key()).copied().unwrap_or_default();
            score_pair(left_score)
                .partial_cmp(&score_pair(right_score))
                .unwrap_or(std::cmp::Ordering::Equal)
        })
        .or_else(|| {
            root_actions.iter().min_by_key(|action| {
                *action
                    .board_after
                    .column_heights()
                    .iter()
                    .max()
                    .unwrap_or(&40)
            })
        })
        .cloned();
    BaseAnalysis {
        chosen,
        root_scores,
        expanded,
    }
}

fn score_pair((value, spike): (f32, f32)) -> f32 {
    value + spike * 0.12
}

fn trim_beam(beam: &mut Vec<BeamNode>, width: usize) {
    beam.sort_by(|left, right| {
        score_pair((right.value, right.spike))
            .partial_cmp(&score_pair((left.value, left.spike)))
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    beam.truncate(width);
}

pub fn base_features(before: &Board, action: &PlacementAction) -> Vec<f32> {
    let after = &action.board_after;
    let before_geometry = BoardGeometry::measure(before);
    let after_geometry = BoardGeometry::measure(after);
    let heights = after.column_heights();
    let sum_height: i32 = heights.iter().sum();
    let top_half: i32 = heights.iter().map(|height| (height - 10).max(0)).sum();
    let top_quarter: i32 = heights.iter().map(|height| (height - 15).max(0)).sum();
    let row_transitions = row_transitions(after);
    let bumpiness_squared: i32 = heights
        .windows(2)
        .map(|pair| (pair[0] - pair[1]).pow(2))
        .sum();
    let (well_depth, max_well_depth, well_column) = well_metrics(after);
    let kind = action.lock.placement_kind;
    let attack = pc0_attack(&action.lock);
    let base_attack = kind.garbage() + action.lock.b2b as u32;
    let combo_attack = attack.saturating_sub(base_attack);
    let is = |candidate| (kind == candidate) as u8 as f32;
    let wasted_t = action.placement.kind.0 == Piece::T
        && !matches!(
            kind,
            PlacementKind::Tspin1 | PlacementKind::Tspin2 | PlacementKind::Tspin3
        );
    let mut features = vec![
        after_geometry.max_height as f32,
        sum_height as f32,
        after_geometry.holes as f32,
        (after_geometry.holes * after_geometry.holes) as f32,
        after_geometry.covered as f32,
        after_geometry.bumpiness as f32,
        bumpiness_squared as f32,
        row_transitions as f32,
        after_geometry.blocks as f32,
        top_half as f32,
        top_quarter as f32,
        well_depth as f32,
        max_well_depth as f32,
        (4.5 - well_column as f32).abs(),
        after.b2b_bonus as u8 as f32,
        after_geometry.bottom_dense_rows as f32,
        after_geometry.accessible_holes as f32,
        after_geometry.max_height as f32 - before_geometry.max_height as f32,
        after_geometry.holes as f32 - before_geometry.holes as f32,
        before_geometry
            .bottom_dense_rows
            .saturating_sub(after_geometry.bottom_dense_rows) as f32,
        attack as f32,
        action.lock.cleared_lines.len() as f32,
        action.lock.combo.unwrap_or(0) as f32,
        combo_attack as f32,
        action.lock.b2b as u8 as f32,
        is(PlacementKind::Clear1),
        is(PlacementKind::Clear2),
        is(PlacementKind::Clear3),
        is(PlacementKind::Clear4),
        is(PlacementKind::Tspin1),
        is(PlacementKind::Tspin2),
        is(PlacementKind::Tspin3),
        action.hold as u8 as f32,
        (action.movements.len() + action.hold as usize + 1) as f32,
        wasted_t as u8 as f32,
        0.0, // PC special reward is unavailable to every KASANE model.
        action.lock.locked_out as u8 as f32,
    ];
    features.extend(heights.iter().map(|height| *height as f32));
    for x in 0..10 {
        let holes = (0..heights[x].max(0))
            .filter(|&y| !after.occupied(x as i32, y))
            .count();
        features.push(holes as f32);
    }
    features.extend(heights.windows(2).map(|pair| (pair[1] - pair[0]) as f32));
    for y in 0..BASE_CELL_ROWS as i32 {
        features.push((0..10).filter(|&x| after.occupied(x, y)).count() as f32);
    }
    // Exact occupancy is deliberately retained in addition to engineered
    // geometry. Row totals alone cannot distinguish where a cheese hole or
    // overhang sits, which made high-level teacher choices non-identifiable.
    for y in 0..BASE_CELL_ROWS as i32 {
        for x in 0..10 {
            features.push(after.occupied(x, y) as u8 as f32);
        }
    }
    append_piece_one_hot(&mut features, Some(action.placement.kind.0));
    features.push(after.hold_piece.is_none() as u8 as f32);
    append_piece_one_hot(&mut features, after.hold_piece);
    let preview: Vec<_> = after.next_queue().take(BASE_PREVIEW_PIECES).collect();
    for index in 0..BASE_PREVIEW_PIECES {
        append_piece_one_hot(&mut features, preview.get(index).copied());
    }
    features.push(action.placement.x as f32);
    features.push(action.placement.y as f32);
    for rotation in [
        RotationState::North,
        RotationState::East,
        RotationState::South,
        RotationState::West,
    ] {
        features.push((action.placement.kind.1 == rotation) as u8 as f32);
    }
    for tspin in [TspinStatus::None, TspinStatus::Mini, TspinStatus::Full] {
        features.push((action.placement.tspin == tspin) as u8 as f32);
    }
    debug_assert_eq!(features.len(), base_feature_names().len());
    features
}

fn append_piece_one_hot(features: &mut Vec<f32>, selected: Option<Piece>) {
    for piece in [
        Piece::I,
        Piece::O,
        Piece::T,
        Piece::L,
        Piece::J,
        Piece::S,
        Piece::Z,
    ] {
        features.push((selected == Some(piece)) as u8 as f32);
    }
}

fn row_transitions(board: &Board) -> u32 {
    let mut transitions = 0;
    for y in 0..20 {
        let mut previous = true;
        for x in 0..10 {
            let occupied = board.occupied(x, y);
            transitions += (occupied != previous) as u32;
            previous = occupied;
        }
        transitions += (!previous) as u32;
    }
    transitions
}

fn well_metrics(board: &Board) -> (u32, u32, usize) {
    let heights = board.column_heights();
    let well = (0..10).min_by_key(|&x| heights[x]).unwrap_or(0);
    let mut total_depth = 0;
    let mut max_depth = 0;
    for y in heights[well].max(0)..20 {
        if (0..10).all(|x| x == well || board.occupied(x as i32, y)) {
            total_depth += 1;
            max_depth = max_depth.max(total_depth);
        } else {
            break;
        }
    }
    (total_depth, max_depth, well)
}

fn dot(left: &[f32], right: &[f32]) -> f32 {
    left.iter().zip(right).map(|(a, b)| a * b).sum()
}

#[cfg(test)]
mod tests {
    use super::*;
    use libtetris::Piece;

    fn opening_board() -> Board {
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
    fn independent_base_search_returns_a_legal_move() {
        let board = opening_board();
        let model = BaseModel::default();
        model.validate().unwrap();
        let analysis = analyze_base(&board, &model, 3, 24, 0);
        assert!(analysis.chosen.is_some());
        assert!(analysis.expanded > legal_actions(&board).len());
        assert!(analysis
            .chosen
            .as_ref()
            .and_then(|action| analysis.root_scores.get(&action.key()))
            .is_some());
    }

    #[test]
    fn base_feature_schema_is_exact() {
        let board = opening_board();
        let action = legal_actions(&board).remove(0);
        assert_eq!(
            base_features(&board, &action).len(),
            base_feature_names().len()
        );
    }
}
