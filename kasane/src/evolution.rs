use crate::agent::{AgentConfig, AgentKind};
use crate::base::{BaseModel, BaseSearchMode};
use crate::game::{InitialField, Match, MatchConfig, MatchOutcome, MatchResult, PlayerSpec};
use crate::model::TempoModel;
use crate::rules::Rules;
use anyhow::{bail, Context, Result};
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

pub const GENOME_NAMES: [&str; 12] = [
    "height_safety",
    "holes_and_cover",
    "surface",
    "well",
    "attack_volume",
    "efficient_clears",
    "small_clear_cost",
    "combo_ren",
    "back_to_back",
    "digging",
    "controller_speed",
    "t_conservation",
];

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct EvolutionConfig {
    pub population: usize,
    pub elites: usize,
    pub generations: usize,
    pub games_per_candidate: usize,
    pub seed: u64,
    pub threads: usize,
    pub time_limit_ms: u64,
    pub defender_nodes: u32,
    pub base_depth: usize,
    pub base_beam_width: usize,
    pub initial_sigma: f64,
    pub sigma_floor: f64,
    pub source_model: PathBuf,
    pub output_model: PathBuf,
    pub output_report: PathBuf,
}

impl Default for EvolutionConfig {
    fn default() -> Self {
        Self {
            population: 16,
            elites: 5,
            generations: 8,
            games_per_candidate: 24,
            seed: 0x4B41_5341_4E45_E701,
            threads: 0,
            time_limit_ms: 20_000,
            defender_nodes: 800,
            base_depth: 4,
            base_beam_width: 64,
            initial_sigma: 0.42,
            sigma_floor: 0.08,
            source_model: PathBuf::from("config/base-model-heuristic-v1.json"),
            output_model: PathBuf::from("models/base-model-evolved-v1.json"),
            output_report: PathBuf::from("results/base-evolution-v1.json"),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct EvolutionEvaluation {
    pub fitness: f64,
    pub ko20: usize,
    pub losses: usize,
    pub timeouts: usize,
    pub mean_end_ms_on_win: Option<f64>,
    pub mean_sent: f64,
    pub mean_cancelled: f64,
    pub mean_max_combo: f64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct EvolutionCandidate {
    pub index: usize,
    pub log_multipliers: Vec<f64>,
    pub multipliers: BTreeMap<String, f64>,
    pub evaluation: EvolutionEvaluation,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct EvolutionGeneration {
    pub generation: usize,
    pub mean_log_multipliers: Vec<f64>,
    pub sigma: Vec<f64>,
    pub candidates: Vec<EvolutionCandidate>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct EvolutionReport {
    pub schema: String,
    pub config: EvolutionConfig,
    pub genome_names: Vec<String>,
    pub generations: Vec<EvolutionGeneration>,
    pub best: EvolutionCandidate,
}

#[derive(Clone)]
struct Sample {
    index: usize,
    genes: Vec<f64>,
    model: BaseModel,
}

pub fn evolve_base(config: EvolutionConfig) -> Result<EvolutionReport> {
    validate_config(&config)?;
    let source = BaseModel::load(&config.source_model)?;
    if source.search_mode != BaseSearchMode::ValueReward {
        bail!("evolution requires a value_reward source model");
    }
    let mut rng = StdRng::seed_from_u64(config.seed);
    let mut mean = vec![0.0_f64; GENOME_NAMES.len()];
    let mut sigma = vec![config.initial_sigma; GENOME_NAMES.len()];
    let mut generations = Vec::with_capacity(config.generations);
    let mut global_best: Option<(EvolutionCandidate, BaseModel)> = None;

    for generation in 0..config.generations {
        let mut samples = Vec::with_capacity(config.population);
        for index in 0..config.population {
            // Candidate zero is the current distribution mean. This gives
            // every generation an unmutated anchor and makes regressions
            // visible instead of relying on noisy elitism alone.
            let genes = if index == 0 {
                mean.clone()
            } else {
                mean.iter()
                    .zip(&sigma)
                    .map(|(&center, &spread)| {
                        (center + spread * standard_normal(&mut rng)).clamp(-1.6, 1.6)
                    })
                    .collect()
            };
            samples.push(Sample {
                index,
                model: apply_genome(&source, &genes),
                genes,
            });
        }

        let evaluations = evaluate_population(&samples, &config)?;
        let mut candidates: Vec<_> = samples
            .iter()
            .zip(evaluations)
            .map(|(sample, evaluation)| EvolutionCandidate {
                index: sample.index,
                multipliers: multiplier_map(&sample.genes),
                log_multipliers: sample.genes.clone(),
                evaluation,
            })
            .collect();
        candidates.sort_by(|left, right| compare_evaluation(&right.evaluation, &left.evaluation));

        let best_generation = candidates[0].clone();
        let best_model = apply_genome(&source, &best_generation.log_multipliers);
        if global_best
            .as_ref()
            .map(|(best, _)| {
                compare_evaluation(&best_generation.evaluation, &best.evaluation)
                    == Ordering::Greater
            })
            .unwrap_or(true)
        {
            global_best = Some((best_generation.clone(), best_model));
        }

        let elite_count = config.elites.min(candidates.len());
        let elites = &candidates[..elite_count];
        for dimension in 0..GENOME_NAMES.len() {
            let next_mean = elites
                .iter()
                .map(|candidate| candidate.log_multipliers[dimension])
                .sum::<f64>()
                / elite_count as f64;
            let variance = elites
                .iter()
                .map(|candidate| (candidate.log_multipliers[dimension] - next_mean).powi(2))
                .sum::<f64>()
                / elite_count as f64;
            // Smooth CEM updates retain exploration when a small noisy cohort
            // happens to agree too strongly in one generation.
            mean[dimension] = 0.25 * mean[dimension] + 0.75 * next_mean;
            sigma[dimension] = (0.30 * sigma[dimension]
                + 0.70 * variance.sqrt().max(config.sigma_floor))
            .max(config.sigma_floor);
        }

        generations.push(EvolutionGeneration {
            generation,
            mean_log_multipliers: mean.clone(),
            sigma: sigma.clone(),
            candidates,
        });
        let (best, model) = global_best.as_ref().unwrap();
        model.save(&config.output_model)?;
        save_report(&config, &generations, best.clone(), &config.output_report)?;
        println!(
            "generation={} fitness={:.3} ko20={}/{} sent={:.3} losses={}",
            generation,
            best_generation.evaluation.fitness,
            best_generation.evaluation.ko20,
            config.games_per_candidate,
            best_generation.evaluation.mean_sent,
            best_generation.evaluation.losses,
        );
    }

    let (best, _) = global_best.expect("at least one generation");
    Ok(EvolutionReport {
        schema: "kasane-base-evolution/v1".to_owned(),
        config,
        genome_names: GENOME_NAMES.iter().map(|name| (*name).to_owned()).collect(),
        generations,
        best,
    })
}

fn validate_config(config: &EvolutionConfig) -> Result<()> {
    if config.population < 2
        || config.elites == 0
        || config.elites > config.population
        || config.generations == 0
        || config.games_per_candidate == 0
    {
        bail!("invalid evolution population/generation configuration");
    }
    if config.time_limit_ms != 20_000 {
        bail!("base evolution is pinned to the primary 20000 ms objective");
    }
    Ok(())
}

fn evaluate_population(
    samples: &[Sample],
    config: &EvolutionConfig,
) -> Result<Vec<EvolutionEvaluation>> {
    let jobs = samples.len() * config.games_per_candidate;
    let run = || {
        (0..jobs)
            .into_par_iter()
            .map(|job| {
                let candidate = job / config.games_per_candidate;
                let game = job % config.games_per_candidate;
                let seed = evaluation_seed(config.seed, game as u64);
                let result = evaluate_match(&samples[candidate].model, config, seed)?;
                Ok((candidate, result))
            })
            .collect::<Result<Vec<_>>>()
    };
    let results = if config.threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(config.threads)
            .build()?
            .install(run)?
    } else {
        run()?
    };
    let mut grouped: Vec<Vec<MatchResult>> = (0..samples.len()).map(|_| Vec::new()).collect();
    for (candidate, result) in results {
        grouped[candidate].push(result);
    }
    Ok(grouped
        .iter()
        .map(|results| summarize_evolution(results, config.time_limit_ms))
        .collect())
}

fn evaluate_match(model: &BaseModel, config: &EvolutionConfig, seed: u64) -> Result<MatchResult> {
    let mut attacker = AgentConfig::default();
    attacker.cold_clear_nodes = config.defender_nodes;
    attacker.base_depth = config.base_depth;
    attacker.base_beam_width = config.base_beam_width;
    attacker.enable_tempo = false;
    attacker.base_model_override = Some(model.clone());
    let mut defender = AgentConfig::default();
    defender.cold_clear_nodes = config.defender_nodes;
    Match::new(MatchConfig {
        rules: Rules::pinned(),
        players: [
            PlayerSpec {
                agent: AgentKind::Kasane,
                initial_field: InitialField::Empty,
            },
            PlayerSpec {
                agent: AgentKind::ColdClear,
                initial_field: InitialField::Cheese {
                    rows: 12,
                    strict_hole_bara: true,
                },
            },
        ],
        agent_configs: [attacker, defender],
        seed,
        time_limit_ms: config.time_limit_ms,
        record_trace: false,
    })?
    .run()
}

fn summarize_evolution(results: &[MatchResult], limit: u64) -> EvolutionEvaluation {
    let mut ko20 = 0;
    let mut losses = 0;
    let mut sent = 0_u64;
    let mut cancelled = 0_u64;
    let mut combo = 0_u64;
    let mut win_time = 0_u64;
    let mut fitness = 0.0;
    for result in results {
        sent += result.stats[0].sent;
        cancelled += result.stats[0].cancelled;
        combo += result.stats[0].max_combo as u64;
        fitness += result.stats[0].sent as f64 * 3.0;
        fitness += result.stats[0].max_combo as f64 * 2.0;
        match result.outcome {
            MatchOutcome::Player0Win => {
                ko20 += 1;
                win_time += result.ended_ms;
                fitness += 10_000.0 + (limit.saturating_sub(result.ended_ms)) as f64 / 4.0;
            }
            MatchOutcome::Player1Win => {
                losses += 1;
                fitness -= 4_000.0;
            }
            MatchOutcome::Draw | MatchOutcome::Timeout => {}
        }
    }
    let count = results.len() as f64;
    EvolutionEvaluation {
        fitness: fitness / count,
        ko20,
        losses,
        timeouts: results.len() - ko20 - losses,
        mean_end_ms_on_win: (ko20 > 0).then_some(win_time as f64 / ko20 as f64),
        mean_sent: sent as f64 / count,
        mean_cancelled: cancelled as f64 / count,
        mean_max_combo: combo as f64 / count,
    }
}

fn compare_evaluation(left: &EvolutionEvaluation, right: &EvolutionEvaluation) -> Ordering {
    left.ko20
        .cmp(&right.ko20)
        .then_with(|| right.losses.cmp(&left.losses))
        .then_with(
            || match (left.mean_end_ms_on_win, right.mean_end_ms_on_win) {
                (Some(left_ms), Some(right_ms)) => {
                    right_ms.partial_cmp(&left_ms).unwrap_or(Ordering::Equal)
                }
                (Some(_), None) => Ordering::Greater,
                (None, Some(_)) => Ordering::Less,
                (None, None) => Ordering::Equal,
            },
        )
        .then_with(|| {
            left.fitness
                .partial_cmp(&right.fitness)
                .unwrap_or(Ordering::Equal)
        })
}

fn apply_genome(source: &BaseModel, genes: &[f64]) -> BaseModel {
    let mut model = source.clone();
    let groups: [&[&str]; 12] = [
        &[
            "max_height",
            "sum_height",
            "top_half",
            "top_quarter",
            "height_delta",
        ],
        &["holes", "holes_squared", "covered", "holes_delta"],
        &["bumpiness", "bumpiness_squared", "row_transitions"],
        &["well_depth", "max_well_depth", "well_center_bias"],
        &["attack_pc0"],
        &["clear4", "tspin2", "tspin3"],
        &["clear1", "clear2", "clear3", "tspin1"],
        &["combo_attack", "combo"],
        &["back_to_back_state", "b2b_clear"],
        &["bottom_dense_rows", "accessible_holes", "dig_delta"],
        &["controller_inputs"],
        &["wasted_t"],
    ];
    for (gene, names) in genes.iter().zip(groups) {
        let multiplier = gene.exp() as f32;
        for name in names {
            if let Some(index) = model
                .feature_names
                .iter()
                .position(|candidate| candidate == name)
            {
                model.linear_weights[index] *= multiplier;
            }
        }
    }
    // PC remains exactly neutral irrespective of optimization.
    if let Some(index) = model
        .feature_names
        .iter()
        .position(|candidate| candidate == "perfect_clear")
    {
        model.linear_weights[index] = 0.0;
    }
    model
}

fn multiplier_map(genes: &[f64]) -> BTreeMap<String, f64> {
    GENOME_NAMES
        .iter()
        .zip(genes)
        .map(|(&name, &gene)| (name.to_owned(), gene.exp()))
        .collect()
}

fn save_report(
    config: &EvolutionConfig,
    generations: &[EvolutionGeneration],
    best: EvolutionCandidate,
    path: &PathBuf,
) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    if let Some(parent) = config.output_model.parent() {
        fs::create_dir_all(parent)?;
    }
    let report = EvolutionReport {
        schema: "kasane-base-evolution/v1".to_owned(),
        config: config.clone(),
        genome_names: GENOME_NAMES.iter().map(|name| (*name).to_owned()).collect(),
        generations: generations.to_vec(),
        best,
    };
    fs::write(path, serde_json::to_vec_pretty(&report)?)
        .with_context(|| format!("failed to write {}", path.display()))?;
    Ok(())
}

fn standard_normal(rng: &mut StdRng) -> f64 {
    // Box-Muller transform; clamp away the open interval endpoint.
    let u1 = rng.gen::<f64>().max(f64::MIN_POSITIVE);
    let u2 = rng.gen::<f64>();
    (-2.0 * u1.ln()).sqrt() * (std::f64::consts::TAU * u2).cos()
}

fn evaluation_seed(seed: u64, index: u64) -> u64 {
    let mut value = seed ^ index.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    value = (value ^ (value >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    value ^ (value >> 31)
}

pub const TEMPO_GENOME_NAMES: [&str; 10] = [
    "base_prior",
    "immediate_pressure",
    "dodge_gain",
    "simultaneous_fire",
    "waiting_cost",
    "two_ply_charge",
    "own_safety",
    "opponent_danger",
    "forecast_risk",
    "base_guard",
];

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct TempoEvolutionConfig {
    pub population: usize,
    pub elites: usize,
    pub generations: usize,
    pub games_per_candidate: usize,
    pub seed: u64,
    pub threads: usize,
    pub time_limit_ms: u64,
    pub defender_nodes: u32,
    pub forecast_nodes: u32,
    pub base_depth: usize,
    pub base_beam_width: usize,
    pub maximum_wait_ms: u64,
    pub initial_sigma: f64,
    pub sigma_floor: f64,
    pub source_base_model: PathBuf,
    pub source_tempo_model: PathBuf,
    pub output_model: PathBuf,
    pub output_report: PathBuf,
}

impl Default for TempoEvolutionConfig {
    fn default() -> Self {
        Self {
            population: 12,
            elites: 4,
            generations: 8,
            games_per_candidate: 16,
            seed: 0x4B41_5341_4E45_7E90,
            threads: 0,
            time_limit_ms: 20_000,
            defender_nodes: 600,
            forecast_nodes: 200,
            base_depth: 3,
            base_beam_width: 32,
            maximum_wait_ms: 1_500,
            initial_sigma: 0.45,
            sigma_floor: 0.08,
            source_base_model: PathBuf::from("models/base-model-evolved-v1.json"),
            source_tempo_model: PathBuf::from("config/tempo-model-bootstrap-v3.json"),
            output_model: PathBuf::from("models/tempo-model-evolved-v1.json"),
            output_report: PathBuf::from("results/tempo-evolution-v1.json"),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TempoEvolutionCandidate {
    pub index: usize,
    pub log_multipliers: Vec<f64>,
    pub multipliers: BTreeMap<String, f64>,
    pub base_guard_margin: f32,
    pub evaluation: EvolutionEvaluation,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TempoEvolutionGeneration {
    pub generation: usize,
    pub mean_log_multipliers: Vec<f64>,
    pub sigma: Vec<f64>,
    pub candidates: Vec<TempoEvolutionCandidate>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TempoEvolutionReport {
    pub schema: String,
    pub config: TempoEvolutionConfig,
    pub genome_names: Vec<String>,
    pub generations: Vec<TempoEvolutionGeneration>,
    pub best: TempoEvolutionCandidate,
}

#[derive(Clone)]
struct TempoSample {
    index: usize,
    genes: Vec<f64>,
    model: TempoModel,
    guard: f32,
}

pub fn evolve_tempo(config: TempoEvolutionConfig) -> Result<TempoEvolutionReport> {
    if config.population < 2
        || config.elites == 0
        || config.elites > config.population
        || config.generations == 0
        || config.games_per_candidate == 0
        || config.time_limit_ms != 20_000
    {
        bail!("invalid or non-pinned tempo evolution configuration");
    }
    let base_model = BaseModel::load(&config.source_base_model)?;
    let source = TempoModel::load(&config.source_tempo_model)?;
    let mut rng = StdRng::seed_from_u64(config.seed);
    let mut mean = vec![0.0_f64; TEMPO_GENOME_NAMES.len()];
    let mut sigma = vec![config.initial_sigma; TEMPO_GENOME_NAMES.len()];
    let mut generations = Vec::with_capacity(config.generations);
    let mut global_best: Option<(TempoEvolutionCandidate, TempoModel)> = None;

    for generation in 0..config.generations {
        let mut samples = Vec::with_capacity(config.population);
        for index in 0..config.population {
            let genes = if index == 0 {
                mean.clone()
            } else {
                mean.iter()
                    .zip(&sigma)
                    .map(|(&center, &spread)| {
                        (center + spread * standard_normal(&mut rng)).clamp(-1.6, 1.6)
                    })
                    .collect()
            };
            let (model, guard) = apply_tempo_genome(&source, &genes);
            samples.push(TempoSample {
                index,
                genes,
                model,
                guard,
            });
        }

        let evaluations = evaluate_tempo_population(&samples, &base_model, &config)?;
        let mut candidates: Vec<_> = samples
            .iter()
            .zip(evaluations)
            .map(|(sample, evaluation)| TempoEvolutionCandidate {
                index: sample.index,
                log_multipliers: sample.genes.clone(),
                multipliers: TEMPO_GENOME_NAMES
                    .iter()
                    .zip(&sample.genes)
                    .map(|(&name, &gene)| (name.to_owned(), gene.exp()))
                    .collect(),
                base_guard_margin: sample.guard,
                evaluation,
            })
            .collect();
        candidates.sort_by(|left, right| compare_evaluation(&right.evaluation, &left.evaluation));
        let best_generation = candidates[0].clone();
        let (best_model, _) = apply_tempo_genome(&source, &best_generation.log_multipliers);
        if global_best
            .as_ref()
            .map(|(best, _)| {
                compare_evaluation(&best_generation.evaluation, &best.evaluation)
                    == Ordering::Greater
            })
            .unwrap_or(true)
        {
            global_best = Some((best_generation.clone(), best_model));
        }

        let elites = &candidates[..config.elites];
        for dimension in 0..TEMPO_GENOME_NAMES.len() {
            let next_mean = elites
                .iter()
                .map(|candidate| candidate.log_multipliers[dimension])
                .sum::<f64>()
                / elites.len() as f64;
            let variance = elites
                .iter()
                .map(|candidate| (candidate.log_multipliers[dimension] - next_mean).powi(2))
                .sum::<f64>()
                / elites.len() as f64;
            mean[dimension] = 0.25 * mean[dimension] + 0.75 * next_mean;
            sigma[dimension] = (0.30 * sigma[dimension]
                + 0.70 * variance.sqrt().max(config.sigma_floor))
            .max(config.sigma_floor);
        }

        generations.push(TempoEvolutionGeneration {
            generation,
            mean_log_multipliers: mean.clone(),
            sigma: sigma.clone(),
            candidates,
        });
        let (best, model) = global_best.as_ref().unwrap();
        if let Some(parent) = config.output_model.parent() {
            fs::create_dir_all(parent)?;
        }
        model.save(&config.output_model)?;
        save_tempo_report(&config, &generations, best.clone())?;
        println!(
            "tempo_generation={} fitness={:.3} ko20={}/{} sent={:.3} guard={:.3}",
            generation,
            best_generation.evaluation.fitness,
            best_generation.evaluation.ko20,
            config.games_per_candidate,
            best_generation.evaluation.mean_sent,
            best_generation.base_guard_margin,
        );
    }

    let (best, _) = global_best.expect("at least one generation");
    Ok(TempoEvolutionReport {
        schema: "kasane-tempo-evolution/v1".to_owned(),
        config,
        genome_names: TEMPO_GENOME_NAMES
            .iter()
            .map(|name| (*name).to_owned())
            .collect(),
        generations,
        best,
    })
}

fn evaluate_tempo_population(
    samples: &[TempoSample],
    base_model: &BaseModel,
    config: &TempoEvolutionConfig,
) -> Result<Vec<EvolutionEvaluation>> {
    let jobs = samples.len() * config.games_per_candidate;
    let run = || {
        (0..jobs)
            .into_par_iter()
            .map(|job| {
                let candidate = job / config.games_per_candidate;
                let game = job % config.games_per_candidate;
                let seed = evaluation_seed(config.seed, game as u64);
                let sample = &samples[candidate];
                let result =
                    evaluate_tempo_match(base_model, &sample.model, sample.guard, config, seed)?;
                Ok((candidate, result))
            })
            .collect::<Result<Vec<_>>>()
    };
    let results = if config.threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(config.threads)
            .build()?
            .install(run)?
    } else {
        run()?
    };
    let mut grouped: Vec<Vec<MatchResult>> = (0..samples.len()).map(|_| Vec::new()).collect();
    for (candidate, result) in results {
        grouped[candidate].push(result);
    }
    Ok(grouped
        .iter()
        .map(|results| summarize_evolution(results, config.time_limit_ms))
        .collect())
}

fn evaluate_tempo_match(
    base_model: &BaseModel,
    tempo_model: &TempoModel,
    guard: f32,
    config: &TempoEvolutionConfig,
    seed: u64,
) -> Result<MatchResult> {
    let mut attacker = AgentConfig::default();
    attacker.cold_clear_nodes = config.defender_nodes;
    attacker.forecast_nodes = config.forecast_nodes;
    attacker.maximum_wait_ms = config.maximum_wait_ms;
    attacker.base_depth = config.base_depth;
    attacker.base_beam_width = config.base_beam_width;
    attacker.enable_tempo = true;
    attacker.enable_tank = false;
    attacker.base_guard_margin = guard;
    attacker.base_model_override = Some(base_model.clone());
    attacker.tempo_model_override = Some(tempo_model.clone());
    let mut defender = AgentConfig::default();
    defender.cold_clear_nodes = config.defender_nodes;
    Match::new(MatchConfig {
        rules: Rules::pinned(),
        players: [
            PlayerSpec {
                agent: AgentKind::Kasane,
                initial_field: InitialField::Empty,
            },
            PlayerSpec {
                agent: AgentKind::ColdClear,
                initial_field: InitialField::Cheese {
                    rows: 12,
                    strict_hole_bara: true,
                },
            },
        ],
        agent_configs: [attacker, defender],
        seed,
        time_limit_ms: config.time_limit_ms,
        record_trace: false,
    })?
    .run()
}

fn apply_tempo_genome(source: &TempoModel, genes: &[f64]) -> (TempoModel, f32) {
    let mut model = source.clone();
    let groups: [&[&str]; 9] = [
        &["base_value", "base_spike"],
        &[
            "raw_attack",
            "sent_now",
            "pressure_after_reply",
            "kill_pressure",
        ],
        &["cancellation_dodge_gain"],
        &["fires_simultaneously"],
        &["action_seconds", "wait_seconds", "clear_delay_seconds"],
        &[
            "charge_attack",
            "charge_dodge_gain",
            "charge_pressure",
            "charge_seconds",
            "charge_feasible",
        ],
        &[
            "own_height",
            "own_holes",
            "own_covered",
            "incoming_at_lock",
            "rise_after_action",
            "safety_margin",
        ],
        &[
            "opponent_height",
            "opponent_holes",
            "opponent_covered",
            "opponent_pending",
        ],
        &[
            "predicted_attack",
            "predicted_outgoing",
            "fires_after_reply",
            "fires_before_reply",
        ],
    ];
    for (gene, names) in genes.iter().take(groups.len()).zip(groups) {
        let multiplier = gene.exp() as f32;
        for name in names {
            if let Some(index) = model
                .feature_names
                .iter()
                .position(|candidate| candidate == name)
            {
                model.linear_weights[index] *= multiplier;
            }
        }
    }
    if let Some(index) = model
        .feature_names
        .iter()
        .position(|candidate| candidate == "perfect_clear")
    {
        model.linear_weights[index] = 0.0;
    }
    let guard = (6.0 * genes[9].exp() as f32).clamp(0.5, 20.0);
    (model, guard)
}

fn save_tempo_report(
    config: &TempoEvolutionConfig,
    generations: &[TempoEvolutionGeneration],
    best: TempoEvolutionCandidate,
) -> Result<()> {
    if let Some(parent) = config.output_report.parent() {
        fs::create_dir_all(parent)?;
    }
    let report = TempoEvolutionReport {
        schema: "kasane-tempo-evolution/v1".to_owned(),
        config: config.clone(),
        genome_names: TEMPO_GENOME_NAMES
            .iter()
            .map(|name| (*name).to_owned())
            .collect(),
        generations: generations.to_vec(),
        best,
    };
    fs::write(&config.output_report, serde_json::to_vec_pretty(&report)?)
        .with_context(|| format!("failed to write {}", config.output_report.display()))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zero_genome_preserves_source_weights() {
        let source = BaseModel::default();
        let mutated = apply_genome(&source, &vec![0.0; GENOME_NAMES.len()]);
        assert_eq!(source.linear_weights, mutated.linear_weights);
    }

    #[test]
    fn perfect_clear_weight_cannot_evolve() {
        let source = BaseModel::default();
        let mutated = apply_genome(&source, &vec![1.0; GENOME_NAMES.len()]);
        let pc = mutated
            .feature_names
            .iter()
            .position(|name| name == "perfect_clear")
            .unwrap();
        assert_eq!(mutated.linear_weights[pc], 0.0);
    }
}
