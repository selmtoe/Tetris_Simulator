use crate::agent::analyze_cold_clear;
use crate::base::{analyze_base, BaseModel};
use crate::base::{base_feature_names, base_features};
use crate::search::{legal_actions, pc0_attack, PlacementAction};
use anyhow::{bail, Context, Result};
use libtetris::Board;
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::PathBuf;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct BaseDatasetConfig {
    pub states: usize,
    pub episode_length: usize,
    pub teacher_nodes: u32,
    pub seed: u64,
    pub threads: usize,
    pub output: PathBuf,
    pub behavior_model_path: Option<PathBuf>,
    pub behavior_probability: f64,
}

impl Default for BaseDatasetConfig {
    fn default() -> Self {
        Self {
            states: 5_000,
            episode_length: 40,
            teacher_nodes: 500,
            seed: 0x4B41_5341_4E45_D157,
            threads: 0,
            output: PathBuf::from("data/base-distillation-v1.jsonl"),
            behavior_model_path: None,
            behavior_probability: 0.70,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct BaseDatasetReport {
    pub schema: String,
    pub states: usize,
    pub candidates: usize,
    pub teacher_nodes: u32,
    pub feature_count: usize,
    pub output: PathBuf,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
struct TrainingGroup {
    features: Vec<Vec<f32>>,
    teacher_scores: Vec<f32>,
    chosen: usize,
    cheese_rows: u32,
    ply: usize,
}

pub fn generate_base_dataset(config: BaseDatasetConfig) -> Result<BaseDatasetReport> {
    if config.states == 0 || config.episode_length == 0 {
        bail!("states and episode_length must be positive");
    }
    if !(0.0..=1.0).contains(&config.behavior_probability) {
        bail!("behavior_probability must be in [0, 1]");
    }
    let behavior_model = config
        .behavior_model_path
        .as_ref()
        .map(BaseModel::load)
        .transpose()?;
    let episodes = config.states.div_ceil(config.episode_length);
    let generate = || {
        (0..episodes)
            .into_par_iter()
            .map(|episode| generate_episode(episode, &config, behavior_model.as_ref()))
            .collect::<Vec<_>>()
    };
    let episode_groups = if config.threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(config.threads)
            .build()?
            .install(generate)
    } else {
        generate()
    };
    let mut groups = Vec::with_capacity(config.states);
    for result in episode_groups {
        groups.extend(result?);
    }
    groups.truncate(config.states);
    let candidates = groups.iter().map(|group| group.features.len()).sum();
    let feature_names = base_feature_names();

    if let Some(parent) = config.output.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let file = File::create(&config.output)
        .with_context(|| format!("failed to create {}", config.output.display()))?;
    let mut writer = BufWriter::new(file);
    serde_json::to_writer(
        &mut writer,
        &json!({
            "type": "metadata",
            "schema": "kasane-base-distillation/v1",
            "feature_names": feature_names,
            "states": groups.len(),
            "candidates": candidates,
            "teacher": "cold-clear-standard-pc0-adjusted",
            "teacher_nodes": config.teacher_nodes,
            "seed": config.seed,
        }),
    )?;
    writer.write_all(b"\n")?;
    for group in &groups {
        serde_json::to_writer(
            &mut writer,
            &json!({
                "type": "group",
                "features": group.features,
                "teacher_scores": group.teacher_scores,
                "chosen": group.chosen,
                "cheese_rows": group.cheese_rows,
                "ply": group.ply,
            }),
        )?;
        writer.write_all(b"\n")?;
    }
    writer.flush()?;
    Ok(BaseDatasetReport {
        schema: "kasane-base-distillation-report/v1".to_owned(),
        states: groups.len(),
        candidates,
        teacher_nodes: config.teacher_nodes,
        feature_count: base_feature_names().len(),
        output: config.output,
    })
}

fn generate_episode(
    episode: usize,
    config: &BaseDatasetConfig,
    behavior_model: Option<&BaseModel>,
) -> Result<Vec<TrainingGroup>> {
    let seed = mix_seed(config.seed, episode as u64);
    let mut rng = StdRng::seed_from_u64(seed);
    let cheese_rows = match episode % 5 {
        0 => 12,
        1 => 8,
        2 => 4,
        _ => 0,
    };
    let mut board = training_board(cheese_rows, &mut rng);
    let mut groups = Vec::with_capacity(config.episode_length);
    for ply in 0..config.episode_length {
        let teacher = analyze_cold_clear(&board, config.teacher_nodes, 0);
        let teacher_choice = teacher.chosen.as_ref().map(PlacementAction::key);
        let actions = legal_actions(&board);
        let mut candidates: Vec<(PlacementAction, f32)> = actions
            .into_iter()
            .filter_map(|action| {
                let (value, spike) = teacher.root_scores.get(&action.key()).copied()?;
                // Cold Clear's DAG stores the 10-line PC override in spike.
                // Remove that override while retaining the ordinary clear,
                // B2B, combo and empty-board geometry value.
                let pc_adjustment = if action.lock.perfect_clear {
                    (10_u32.saturating_sub(pc0_attack(&action.lock))) as f32 * 0.1
                } else {
                    0.0
                };
                Some((action, value as f32 + spike as f32 * 0.1 - pc_adjustment))
            })
            .filter(|(action, _)| !action.lock.locked_out)
            .collect();
        if candidates.len() < 2 {
            break;
        }
        let Some(chosen) = candidates
            .iter()
            .position(|(action, _)| Some(action.key()) == teacher_choice)
        else {
            break;
        };
        groups.push(TrainingGroup {
            features: candidates
                .iter()
                .map(|(action, _)| base_features(&board, action))
                .collect(),
            teacher_scores: candidates.iter().map(|(_, score)| *score).collect(),
            chosen,
            cheese_rows,
            ply,
        });

        // DAgger-style state diversification: mostly follow the teacher, but
        // deliberately enter plausible suboptimal states and teach recovery.
        let behavior_branch = behavior_model.and_then(|model| {
            if rng.gen::<f64>() >= config.behavior_probability {
                return None;
            }
            let selected = analyze_base(&board, model, 1, 1, 0).chosen?;
            candidates
                .iter()
                .position(|(action, _)| action.key() == selected.key())
        });
        let branch = if let Some(branch) = behavior_branch {
            branch
        } else if rng.gen::<f64>() < 0.82 {
            chosen
        } else {
            rng.gen_range(0, candidates.len())
        };
        let action = candidates.swap_remove(branch).0;
        board = action.board_after;
        for _ in 0..action.pieces_consumed {
            let piece = board.generate_next_piece(&mut rng);
            board.add_next_piece(piece);
        }
        if rng.gen::<f64>() < 0.12 && *board.column_heights().iter().max().unwrap_or(&40) < 14 {
            let lines = rng.gen_range(1, 5);
            let mut previous = None;
            for _ in 0..lines {
                let hole = random_different_hole(previous, &mut rng);
                previous = Some(hole);
                if board.add_garbage(hole) {
                    break;
                }
            }
        }
    }
    Ok(groups)
}

fn training_board(cheese_rows: u32, rng: &mut StdRng) -> Board {
    let mut board = Board::new();
    if cheese_rows > 0 {
        let mut field = [[false; 10]; 40];
        let mut previous = None;
        for row in field.iter_mut().take(cheese_rows as usize) {
            let hole = random_different_hole(previous, rng);
            previous = Some(hole);
            for (x, cell) in row.iter_mut().enumerate() {
                *cell = x != hole;
            }
        }
        board.set_field(field);
    }
    for _ in 0..12 {
        let piece = board.generate_next_piece(rng);
        board.add_next_piece(piece);
    }
    board
}

fn random_different_hole(previous: Option<usize>, rng: &mut StdRng) -> usize {
    match previous {
        None => rng.gen_range(0, 10),
        Some(previous) => {
            let value = rng.gen_range(0, 9);
            if value >= previous {
                value + 1
            } else {
                value
            }
        }
    }
}

fn mix_seed(seed: u64, stream: u64) -> u64 {
    let mut value = seed ^ stream.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    value = (value ^ (value >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    value ^ (value >> 31)
}
