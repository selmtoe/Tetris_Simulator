//! Three-way-split CEM training for the CC-floor strategic policy.
//!
//! The candidate and baseline share every seed and ruleset.  Training mixes
//! the original 12-row strict-cheese finishing task with an empty-field direct
//! duel, so offensive cancellation dodge and defensive post-fire countering
//! cannot be optimized by a single proxy statistic.

use crate::agent::{AgentConfig, AgentKind};
use crate::base::BaseModel;
use crate::game::{InitialField, Match, MatchConfig, MatchOutcome, MatchResult, PlayerSpec};
use crate::model::TempoModel;
use crate::rules::Rules;
use anyhow::{bail, Context, Result};
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Instant;

pub const STRATEGY_GENOME_GROUPS: [&str; 16] = [
    "cc_floor_prior",
    "offensive_pressure",
    "defensive_counter",
    "own_survival",
    "opponent_lethality",
    "forecast_risk",
    "next_attack_resource",
    "ren_resource",
    "dig_resource",
    "tempo_cost",
    "geometry_improvement",
    "alternative_cost",
    "block_resource_balance",
    "legacy_attack",
    "incoming_deadlines",
    "synchronization",
];

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct StrategyTrainingConfig {
    pub population: usize,
    pub elites: usize,
    pub generations: usize,
    pub train_games: usize,
    pub validation_games: usize,
    pub holdout_games: usize,
    pub train_seed: u64,
    pub validation_seed: u64,
    pub holdout_seed: u64,
    pub nodes: u32,
    pub forecast_nodes: u32,
    pub time_limit_ms: u64,
    pub threads: usize,
    pub initial_sigma: f64,
    pub sigma_floor: f64,
    pub source_base_model: PathBuf,
    pub source_strategy_model: Option<PathBuf>,
    pub output_model: PathBuf,
    pub output_policy: PathBuf,
    pub output_report: PathBuf,
    pub strategy_max_alternatives: usize,
    pub strategy_max_wait_ms: u64,
    pub initial_min_advantage: f32,
    pub placement_min_advantage: f32,
}

impl Default for StrategyTrainingConfig {
    fn default() -> Self {
        Self {
            population: 10,
            elites: 3,
            generations: 5,
            train_games: 10,
            validation_games: 12,
            holdout_games: 40,
            train_seed: 0x4B41_5341_4E45_7201,
            validation_seed: 0x4B41_5341_4E45_7202,
            holdout_seed: 0x4B41_5341_4E45_7203,
            nodes: 400,
            forecast_nodes: 160,
            time_limit_ms: 20_000,
            threads: 0,
            initial_sigma: 0.35,
            sigma_floor: 0.06,
            source_base_model: PathBuf::from("models/base-model-evolved-v3.json"),
            source_strategy_model: None,
            output_model: PathBuf::from("models/strategy-model-v2.json"),
            output_policy: PathBuf::from("config/kasane-strategy-v2.json"),
            output_report: PathBuf::from("results/strategy-training-v2.json"),
            strategy_max_alternatives: 16,
            strategy_max_wait_ms: 1_000,
            initial_min_advantage: 0.35,
            placement_min_advantage: 3.0,
        }
    }
}

impl StrategyTrainingConfig {
    fn validate(&self) -> Result<()> {
        if self.population < 2
            || self.elites == 0
            || self.elites > self.population
            || self.generations == 0
            || self.train_games == 0
            || self.validation_games == 0
            || self.holdout_games == 0
            || self.nodes == 0
            || self.forecast_nodes == 0
            || self.time_limit_ms == 0
            || self.strategy_max_alternatives == 0
            || self.strategy_max_wait_ms == 0
            || !self.initial_sigma.is_finite()
            || self.initial_sigma <= 0.0
            || !self.sigma_floor.is_finite()
            || self.sigma_floor <= 0.0
            || !self.initial_min_advantage.is_finite()
            || self.initial_min_advantage < 0.0
            || !self.placement_min_advantage.is_finite()
            || self.placement_min_advantage < 0.0
        {
            bail!("invalid strategy training configuration");
        }
        if self.train_seed == self.validation_seed
            || self.train_seed == self.holdout_seed
            || self.validation_seed == self.holdout_seed
        {
            bail!("train, validation and holdout seed roots must differ");
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct StrategyEvaluation {
    pub fitness: f64,
    pub games: usize,
    pub offense_cc_ko: usize,
    pub offense_strategy_ko: usize,
    pub offense_ko_delta: i64,
    pub duel_cc_wins: usize,
    pub duel_cc_losses: usize,
    pub duel_strategy_wins: usize,
    pub duel_strategy_losses: usize,
    pub duel_score_delta: i64,
    pub mean_sent_delta: f64,
    pub mean_cancelled_delta: f64,
    pub mean_combo_delta: f64,
    pub mean_policy_overrides: f64,
    pub mean_placement_overrides: f64,
    pub mean_wait_overrides: f64,
    pub mean_dodge_overrides: f64,
    pub mean_counter_overrides: f64,
    pub mean_charge_entries: f64,
    pub mean_charge_actions: f64,
    pub mean_armed_actions: f64,
    pub mean_releases: f64,
    pub mean_release_dodges: f64,
    pub mean_release_counters: f64,
    pub mean_charge_aborts: f64,
    pub mean_charge_overrides: f64,
    pub mean_release_overrides: f64,
    /// Native end-to-end compute for both players divided by strategy pieces.
    pub mean_compute_ms_per_strategy_piece: f64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StrategyCandidate {
    pub index: usize,
    pub genes: Vec<f64>,
    pub min_advantage: f32,
    pub train: StrategyEvaluation,
    pub validation: Option<StrategyEvaluation>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StrategyGeneration {
    pub generation: usize,
    pub mean: Vec<f64>,
    pub sigma: Vec<f64>,
    pub candidates: Vec<StrategyCandidate>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StrategyPolicyArtifact {
    pub schema: String,
    pub base_model: PathBuf,
    pub strategy_model: PathBuf,
    pub attack_expert_model: PathBuf,
    pub cold_clear_floor_nodes: u32,
    pub forecast_nodes: u32,
    pub maximum_wait_ms: u64,
    pub enable_tank: bool,
    #[serde(default)]
    pub strict_base_policy: bool,
    pub strategy_min_advantage: f32,
    pub strategy_placement_min_advantage: f32,
    pub strategy_max_alternatives: usize,
    pub strategy_max_wait_ms: u64,
    pub strategy_enable_stateful: bool,
    pub strategy_charge_min_pieces: u32,
    pub strategy_charge_max_pieces: u32,
    pub strategy_charge_min_headroom: i32,
    pub strategy_release_window_ms: u64,
    pub strategy_counter_offset_ms: u64,
    pub strategy_enable_moe: bool,
    pub strategy_attack_enter_height: u32,
    pub strategy_attack_hard_finish_height: u32,
    pub strategy_attack_stay_height: u32,
    pub strategy_attack_hole_burden: u32,
    pub strategy_attack_min_headroom: i32,
    pub strategy_attack_max_due_1000: u32,
    pub strategy_enable_ren: bool,
    pub strategy_ren_depth: usize,
    pub strategy_ren_beam_width: usize,
    pub strategy_ren_start_chain: u32,
    pub strategy_ren_min_headroom: i32,
    pub hold_fire_requires_sent_gain: bool,
    pub charge_release_requires_observed_edge: bool,
    pub pc_special_attack: u32,
}

impl StrategyPolicyArtifact {
    /// Apply every learned/stateful policy field. Callers may deliberately
    /// override node budgets after this method so one policy can be evaluated
    /// at CC300, CC1500, and the 120k production budget without silently
    /// falling back to `AgentConfig` defaults.
    pub fn apply_to(&self, config: &mut AgentConfig) {
        config.cold_clear_nodes = self.cold_clear_floor_nodes;
        config.kasane_nodes = self.cold_clear_floor_nodes;
        config.forecast_nodes = self.forecast_nodes;
        config.maximum_wait_ms = self.maximum_wait_ms;
        config.enable_tank = self.enable_tank;
        config.strict_base_policy = self.strict_base_policy;
        config.base_model_path = Some(self.base_model.clone());
        config.tempo_model_path = Some(self.strategy_model.clone());
        config.enable_tempo = true;
        config.strategy_min_advantage = self.strategy_min_advantage;
        config.strategy_placement_min_advantage = self.strategy_placement_min_advantage;
        config.strategy_max_alternatives = self.strategy_max_alternatives;
        config.strategy_max_wait_ms = self.strategy_max_wait_ms;
        config.strategy_enable_stateful = self.strategy_enable_stateful;
        config.strategy_charge_min_pieces = self.strategy_charge_min_pieces;
        config.strategy_charge_max_pieces = self.strategy_charge_max_pieces;
        config.strategy_charge_min_headroom = self.strategy_charge_min_headroom;
        config.strategy_release_window_ms = self.strategy_release_window_ms;
        config.strategy_counter_offset_ms = self.strategy_counter_offset_ms;
        config.strategy_enable_moe = self.strategy_enable_moe;
        config.strategy_attack_enter_height = self.strategy_attack_enter_height;
        config.strategy_attack_hard_finish_height = self.strategy_attack_hard_finish_height;
        config.strategy_attack_stay_height = self.strategy_attack_stay_height;
        config.strategy_attack_hole_burden = self.strategy_attack_hole_burden;
        config.strategy_attack_min_headroom = self.strategy_attack_min_headroom;
        config.strategy_attack_max_due_1000 = self.strategy_attack_max_due_1000;
        config.strategy_enable_ren = self.strategy_enable_ren;
        config.strategy_ren_depth = self.strategy_ren_depth;
        config.strategy_ren_beam_width = self.strategy_ren_beam_width;
        config.strategy_ren_start_chain = self.strategy_ren_start_chain;
        config.strategy_ren_min_headroom = self.strategy_ren_min_headroom;
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StrategyTrainingReport {
    pub schema: String,
    pub contract: String,
    pub config: StrategyTrainingConfig,
    pub genome_names: Vec<String>,
    pub generations: Vec<StrategyGeneration>,
    pub selected_generation: usize,
    pub selected_candidate: StrategyCandidate,
    pub holdout: StrategyEvaluation,
    pub total_wall_seconds: f64,
}

#[derive(Clone)]
struct Sample {
    index: usize,
    genes: Vec<f64>,
    model: TempoModel,
    min_advantage: f32,
}

#[derive(Clone)]
struct BaselinePair {
    offense: MatchResult,
    duel: MatchResult,
}

#[derive(Clone)]
struct CandidatePair {
    offense: MatchResult,
    duel: MatchResult,
    wall_seconds: f64,
}

pub fn train_strategy_v2(config: StrategyTrainingConfig) -> Result<StrategyTrainingReport> {
    config.validate()?;
    let started = Instant::now();
    let base_model = BaseModel::load(&config.source_base_model)?;
    let source = match &config.source_strategy_model {
        Some(path) => TempoModel::load(path)?,
        None => TempoModel::strategy_v2_bootstrap(),
    };
    if !source.is_strategy_v2() {
        bail!("source strategy model is not a v2 CC-floor model");
    }
    let train_seeds = split_seeds(config.train_seed, config.train_games);
    let validation_seeds = split_seeds(config.validation_seed, config.validation_games);
    let holdout_seeds = split_seeds(config.holdout_seed, config.holdout_games);

    eprintln!("strategy-v2: evaluating paired train/validation CC floors");
    let train_baseline = evaluate_baseline(&config, &train_seeds)?;
    let validation_baseline = evaluate_baseline(&config, &validation_seeds)?;
    let holdout_baseline = evaluate_baseline(&config, &holdout_seeds)?;

    let dimensions = STRATEGY_GENOME_GROUPS.len() + source.neural_output_weights.len() + 1;
    let mut mean = vec![0.0; dimensions];
    let mut sigma = vec![config.initial_sigma; dimensions];
    let mut rng = StdRng::seed_from_u64(config.train_seed ^ 0xC3E5_7202);
    let mut generations = Vec::with_capacity(config.generations);
    let mut selected: Option<(usize, StrategyCandidate, TempoModel)> = None;

    for generation in 0..config.generations {
        let samples: Vec<_> = (0..config.population)
            .map(|index| {
                let genes = if index <= 1 {
                    mean.clone()
                } else {
                    mean.iter()
                        .zip(&sigma)
                        .map(|(&center, &spread)| {
                            (center + spread * standard_normal(&mut rng)).clamp(-1.8, 1.8)
                        })
                        .collect()
                };
                let (model, learned_min_advantage) =
                    apply_strategy_genome(&source, &genes, &config);
                // Candidate zero is an exact equal-node CC floor control in
                // every generation. It prevents validation noise from forcing
                // selection of a policy that is worse than doing no override.
                let min_advantage = if index == 0 {
                    1_000.0
                } else {
                    learned_min_advantage
                };
                Sample {
                    index,
                    genes,
                    model,
                    min_advantage,
                }
            })
            .collect();
        let train_evaluations = evaluate_population(
            &samples,
            &base_model,
            &config,
            &train_seeds,
            &train_baseline,
        )?;
        let mut candidates: Vec<_> = samples
            .iter()
            .zip(train_evaluations)
            .map(|(sample, train)| StrategyCandidate {
                index: sample.index,
                genes: sample.genes.clone(),
                min_advantage: sample.min_advantage,
                train,
                validation: None,
            })
            .collect();
        candidates.sort_by(|left, right| compare_evaluation(&right.train, &left.train));

        // Validation never updates the CEM distribution.  It only chooses
        // among this generation's train elites and the final checkpoint.
        let mut validation_indices: Vec<_> = candidates
            .iter()
            .take(config.elites)
            .map(|candidate| candidate.index)
            .collect();
        if !validation_indices.contains(&0) {
            validation_indices.push(0);
        }
        for candidate in candidates
            .iter_mut()
            .filter(|candidate| validation_indices.contains(&candidate.index))
        {
            let sample = samples
                .iter()
                .find(|sample| sample.index == candidate.index)
                .expect("sample index is stable");
            candidate.validation = Some(evaluate_model(
                &sample.model,
                sample.min_advantage,
                &base_model,
                &config,
                &validation_seeds,
                &validation_baseline,
            )?);
        }
        let validation_best = candidates
            .iter()
            .filter(|candidate| candidate.validation.is_some())
            .max_by(|left, right| compare_checkpoint(left, right))
            .unwrap()
            .clone();
        let validation_model = samples
            .iter()
            .find(|sample| sample.index == validation_best.index)
            .unwrap()
            .model
            .clone();
        let replace = selected
            .as_ref()
            .map(|(_, best, _)| compare_checkpoint(&validation_best, best) == Ordering::Greater)
            .unwrap_or(true);
        if replace {
            selected = Some((generation, validation_best.clone(), validation_model));
        }

        let train_elites = &candidates[..config.elites];
        for dimension in 0..dimensions {
            let elite_mean = train_elites
                .iter()
                .map(|candidate| candidate.genes[dimension])
                .sum::<f64>()
                / train_elites.len() as f64;
            let variance = train_elites
                .iter()
                .map(|candidate| (candidate.genes[dimension] - elite_mean).powi(2))
                .sum::<f64>()
                / train_elites.len() as f64;
            mean[dimension] = 0.30 * mean[dimension] + 0.70 * elite_mean;
            sigma[dimension] = (0.35 * sigma[dimension]
                + 0.65 * variance.sqrt().max(config.sigma_floor))
            .max(config.sigma_floor);
        }

        eprintln!(
            "strategy_generation={} train_fitness={:.2} validation_fitness={:.2} train_ko_delta={} validation_ko_delta={} overrides={:.3}",
            generation,
            candidates[0].train.fitness,
            validation_best.validation.as_ref().unwrap().fitness,
            candidates[0].train.offense_ko_delta,
            validation_best.validation.as_ref().unwrap().offense_ko_delta,
            validation_best
                .validation
                .as_ref()
                .unwrap()
                .mean_policy_overrides,
        );
        generations.push(StrategyGeneration {
            generation,
            mean: mean.clone(),
            sigma: sigma.clone(),
            candidates,
        });
    }

    let (selected_generation, mut selected_candidate, selected_model) =
        selected.expect("at least one generation");
    let holdout = evaluate_model(
        &selected_model,
        selected_candidate.min_advantage,
        &base_model,
        &config,
        &holdout_seeds,
        &holdout_baseline,
    )?;
    save_parent(&config.output_model)?;
    selected_model.save(&config.output_model)?;
    let policy = StrategyPolicyArtifact {
        schema: "kasane-strategy-policy/v2-moe".to_owned(),
        base_model: config.source_base_model.clone(),
        strategy_model: config.output_model.clone(),
        attack_expert_model: PathBuf::from("config/tempo-model-bootstrap-v3.json"),
        cold_clear_floor_nodes: config.nodes,
        forecast_nodes: config.forecast_nodes,
        maximum_wait_ms: config.strategy_max_wait_ms,
        enable_tank: true,
        strict_base_policy: false,
        strategy_min_advantage: selected_candidate.min_advantage,
        strategy_placement_min_advantage: config.placement_min_advantage,
        strategy_max_alternatives: config.strategy_max_alternatives,
        strategy_max_wait_ms: config.strategy_max_wait_ms,
        strategy_enable_stateful: true,
        strategy_charge_min_pieces: 2,
        strategy_charge_max_pieces: 16,
        strategy_charge_min_headroom: 10,
        strategy_release_window_ms: 750,
        strategy_counter_offset_ms: 50,
        strategy_enable_moe: true,
        strategy_attack_enter_height: 8,
        strategy_attack_hard_finish_height: 11,
        strategy_attack_stay_height: 6,
        strategy_attack_hole_burden: 6,
        strategy_attack_min_headroom: 8,
        strategy_attack_max_due_1000: 6,
        strategy_enable_ren: true,
        strategy_ren_depth: 6,
        strategy_ren_beam_width: 20,
        strategy_ren_start_chain: 6,
        strategy_ren_min_headroom: 6,
        hold_fire_requires_sent_gain: true,
        charge_release_requires_observed_edge: true,
        pc_special_attack: 0,
    };
    save_json(&config.output_policy, &policy)?;
    selected_candidate.validation = selected_candidate.validation.take();
    let report = StrategyTrainingReport {
        schema: "kasane-strategy-training/v2-three-way-paired".to_owned(),
        contract: "Mixture of experts: equal-node Cold Clear is the ordinary/survival floor and Base-v3 + strict legacy Tempo is the trusted finishing expert. Hold-fire changes only lock timing and requires sent gain; charge-then-release uses multi-piece non-firing state and may offensively release only on an observed garbage-rise edge. Train/validation/holdout seed roots are disjoint; bottom12 PC special attack/reward is zero."
            .to_owned(),
        config: config.clone(),
        genome_names: genome_names(&source),
        generations,
        selected_generation,
        selected_candidate,
        holdout,
        total_wall_seconds: started.elapsed().as_secs_f64(),
    };
    save_json(&config.output_report, &report)?;
    Ok(report)
}

fn evaluate_baseline(config: &StrategyTrainingConfig, seeds: &[u64]) -> Result<Vec<BaselinePair>> {
    run_parallel(config.threads, seeds, |seed| {
        Ok(BaselinePair {
            offense: run_match(*seed, AgentKind::ColdClear, None, 0.0, config, true)?,
            duel: run_match(*seed, AgentKind::ColdClear, None, 0.0, config, false)?,
        })
    })
}

fn evaluate_population(
    samples: &[Sample],
    base_model: &BaseModel,
    config: &StrategyTrainingConfig,
    seeds: &[u64],
    baseline: &[BaselinePair],
) -> Result<Vec<StrategyEvaluation>> {
    let jobs: Vec<_> = (0..samples.len())
        .flat_map(|candidate| (0..seeds.len()).map(move |game| (candidate, game)))
        .collect();
    let pairs = run_parallel(config.threads, &jobs, |&(candidate, game)| {
        let sample = &samples[candidate];
        let started = Instant::now();
        let offense = run_match(
            seeds[game],
            AgentKind::Kasane,
            Some((base_model, &sample.model)),
            sample.min_advantage,
            config,
            true,
        )?;
        let duel = run_match(
            seeds[game],
            AgentKind::Kasane,
            Some((base_model, &sample.model)),
            sample.min_advantage,
            config,
            false,
        )?;
        Ok((
            candidate,
            game,
            CandidatePair {
                offense,
                duel,
                wall_seconds: started.elapsed().as_secs_f64(),
            },
        ))
    })?;
    let mut grouped: Vec<Vec<Option<CandidatePair>>> = (0..samples.len())
        .map(|_| vec![None; seeds.len()])
        .collect();
    for (candidate, game, pair) in pairs {
        grouped[candidate][game] = Some(pair);
    }
    grouped
        .into_iter()
        .map(|pairs| {
            let pairs: Vec<_> = pairs.into_iter().map(Option::unwrap).collect();
            Ok(summarize(baseline, &pairs))
        })
        .collect()
}

fn evaluate_model(
    model: &TempoModel,
    min_advantage: f32,
    base_model: &BaseModel,
    config: &StrategyTrainingConfig,
    seeds: &[u64],
    baseline: &[BaselinePair],
) -> Result<StrategyEvaluation> {
    let pairs = run_parallel(config.threads, seeds, |seed| {
        let started = Instant::now();
        let offense = run_match(
            *seed,
            AgentKind::Kasane,
            Some((base_model, model)),
            min_advantage,
            config,
            true,
        )?;
        let duel = run_match(
            *seed,
            AgentKind::Kasane,
            Some((base_model, model)),
            min_advantage,
            config,
            false,
        )?;
        Ok(CandidatePair {
            offense,
            duel,
            wall_seconds: started.elapsed().as_secs_f64(),
        })
    })?;
    Ok(summarize(baseline, &pairs))
}

fn run_match(
    seed: u64,
    attacker: AgentKind,
    models: Option<(&BaseModel, &TempoModel)>,
    min_advantage: f32,
    config: &StrategyTrainingConfig,
    cheese_defender: bool,
) -> Result<MatchResult> {
    let mut own = AgentConfig::default();
    own.cold_clear_nodes = config.nodes;
    own.kasane_nodes = config.nodes;
    own.forecast_nodes = config.forecast_nodes;
    own.maximum_wait_ms = config.strategy_max_wait_ms;
    own.strategy_max_wait_ms = config.strategy_max_wait_ms;
    own.strategy_max_alternatives = config.strategy_max_alternatives;
    own.strategy_min_advantage = min_advantage;
    own.strategy_placement_min_advantage = config.placement_min_advantage;
    own.enable_tank = true;
    own.strategy_enable_stateful = true;
    own.strategy_charge_min_pieces = 2;
    own.strategy_charge_max_pieces = 10;
    own.strategy_charge_min_headroom = 10;
    own.strategy_release_window_ms = 750;
    own.strategy_counter_offset_ms = 50;
    if let Some((base, strategy)) = models {
        own.base_model_override = Some(base.clone());
        own.tempo_model_override = Some(strategy.clone());
    }
    let mut opponent = AgentConfig::default();
    opponent.cold_clear_nodes = config.nodes;
    Match::new(MatchConfig {
        rules: Rules::pinned(),
        players: [
            PlayerSpec {
                agent: attacker,
                initial_field: InitialField::Empty,
            },
            PlayerSpec {
                agent: AgentKind::ColdClear,
                initial_field: if cheese_defender {
                    InitialField::Cheese {
                        rows: 12,
                        strict_hole_bara: true,
                    }
                } else {
                    InitialField::Empty
                },
            },
        ],
        agent_configs: [own, opponent],
        seed,
        time_limit_ms: config.time_limit_ms,
        record_trace: false,
    })?
    .run()
}

fn summarize(baseline: &[BaselinePair], strategy: &[CandidatePair]) -> StrategyEvaluation {
    let games = baseline.len().min(strategy.len());
    let mut result = StrategyEvaluation {
        games,
        ..StrategyEvaluation::default()
    };
    let mut sent_delta = 0.0;
    let mut cancelled_delta = 0.0;
    let mut combo_delta = 0.0;
    let mut overrides = 0_u64;
    let mut placement_overrides = 0_u64;
    let mut wait_overrides = 0_u64;
    let mut dodge_overrides = 0_u64;
    let mut counter_overrides = 0_u64;
    let mut charge_entries = 0_u64;
    let mut charge_actions = 0_u64;
    let mut armed_actions = 0_u64;
    let mut releases = 0_u64;
    let mut release_dodges = 0_u64;
    let mut release_counters = 0_u64;
    let mut charge_aborts = 0_u64;
    let mut charge_overrides = 0_u64;
    let mut release_overrides = 0_u64;
    let mut compute_seconds = 0.0;
    let mut strategy_pieces = 0_u64;
    for (cc, candidate) in baseline.iter().zip(strategy) {
        let cc_offense_ko = cc.offense.outcome == MatchOutcome::Player0Win;
        let strategy_offense_ko = candidate.offense.outcome == MatchOutcome::Player0Win;
        result.offense_cc_ko += cc_offense_ko as usize;
        result.offense_strategy_ko += strategy_offense_ko as usize;
        let cc_duel = outcome_score(&cc.duel);
        let strategy_duel = outcome_score(&candidate.duel);
        result.duel_cc_wins += (cc_duel > 0) as usize;
        result.duel_cc_losses += (cc_duel < 0) as usize;
        result.duel_strategy_wins += (strategy_duel > 0) as usize;
        result.duel_strategy_losses += (strategy_duel < 0) as usize;
        for (base_match, strategy_match) in [
            (&cc.offense, &candidate.offense),
            (&cc.duel, &candidate.duel),
        ] {
            sent_delta += strategy_match.stats[0].sent as f64 - base_match.stats[0].sent as f64;
            cancelled_delta +=
                strategy_match.stats[0].cancelled as f64 - base_match.stats[0].cancelled as f64;
            combo_delta +=
                strategy_match.stats[0].max_combo as f64 - base_match.stats[0].max_combo as f64;
            overrides += strategy_match.stats[0].policy_overrides;
            placement_overrides += strategy_match.stats[0].placement_overrides;
            wait_overrides += strategy_match.stats[0].wait_overrides;
            dodge_overrides += strategy_match.stats[0].dodge_overrides;
            counter_overrides += strategy_match.stats[0].counter_overrides;
            charge_entries += strategy_match.stats[0].charge_entries;
            charge_actions += strategy_match.stats[0].charge_actions;
            armed_actions += strategy_match.stats[0].armed_actions;
            releases += strategy_match.stats[0].releases;
            release_dodges += strategy_match.stats[0].release_dodges;
            release_counters += strategy_match.stats[0].release_counters;
            charge_aborts += strategy_match.stats[0].charge_aborts;
            charge_overrides += strategy_match.stats[0].charge_overrides;
            release_overrides += strategy_match.stats[0].release_overrides;
            strategy_pieces += strategy_match.stats[0].pieces;
        }
        compute_seconds += candidate.wall_seconds;
    }
    result.offense_ko_delta = result.offense_strategy_ko as i64 - result.offense_cc_ko as i64;
    result.duel_score_delta = result.duel_strategy_wins as i64
        - result.duel_strategy_losses as i64
        - (result.duel_cc_wins as i64 - result.duel_cc_losses as i64);
    let paired_games = games.max(1) as f64;
    let matches = paired_games * 2.0;
    result.mean_sent_delta = sent_delta / matches;
    result.mean_cancelled_delta = cancelled_delta / matches;
    result.mean_combo_delta = combo_delta / matches;
    result.mean_policy_overrides = overrides as f64 / matches;
    result.mean_placement_overrides = placement_overrides as f64 / matches;
    result.mean_wait_overrides = wait_overrides as f64 / matches;
    result.mean_dodge_overrides = dodge_overrides as f64 / matches;
    result.mean_counter_overrides = counter_overrides as f64 / matches;
    result.mean_charge_entries = charge_entries as f64 / matches;
    result.mean_charge_actions = charge_actions as f64 / matches;
    result.mean_armed_actions = armed_actions as f64 / matches;
    result.mean_releases = releases as f64 / matches;
    result.mean_release_dodges = release_dodges as f64 / matches;
    result.mean_release_counters = release_counters as f64 / matches;
    result.mean_charge_aborts = charge_aborts as f64 / matches;
    result.mean_charge_overrides = charge_overrides as f64 / matches;
    result.mean_release_overrides = release_overrides as f64 / matches;
    result.mean_compute_ms_per_strategy_piece =
        compute_seconds * 1000.0 / strategy_pieces.max(1) as f64;
    result.fitness = result.offense_ko_delta as f64 * 2_000.0 / paired_games
        + result.duel_score_delta as f64 * 1_800.0 / paired_games
        + result.mean_sent_delta * 16.0
        + result.mean_cancelled_delta * 18.0
        + result.mean_combo_delta * 6.0
        + result.mean_dodge_overrides * 1.5
        + result.mean_counter_overrides * 1.5
        + result.mean_release_dodges * 1.0
        + result.mean_release_counters * 1.0
        - result.mean_charge_aborts * 0.5
        - result.mean_compute_ms_per_strategy_piece * 0.02;
    result
}

fn outcome_score(result: &MatchResult) -> i32 {
    match result.outcome {
        MatchOutcome::Player0Win => 1,
        MatchOutcome::Player1Win => -1,
        MatchOutcome::Draw | MatchOutcome::Timeout => 0,
    }
}

fn apply_strategy_genome(
    source: &TempoModel,
    genes: &[f64],
    config: &StrategyTrainingConfig,
) -> (TempoModel, f32) {
    let mut model = source.clone();
    let groups: [&[&str]; 16] = [
        &[
            "baseline_same_action",
            "base_value_delta",
            "base_spike_delta",
        ],
        &[
            "sent_delta_vs_cc",
            "pressure_delta_vs_cc",
            "lethal_overflow",
            "attack_per_second",
            "pressure_per_second",
        ],
        &[
            "cancel_delta_vs_cc",
            "cancelled_due_500",
            "uncancelled_due_500",
        ],
        &[
            "own_headroom_after_rise",
            "survival_risk",
            "rise_delta_vs_cc",
            "charge_safety_score",
        ],
        &[
            "opponent_headroom_after_pressure",
            "opponent_dig_burden",
            "kill_pressure",
        ],
        &[
            "forecast_confidence",
            "forecast_attack_500",
            "forecast_attack_1000",
            "forecast_outgoing_500",
            "forecast_outgoing_1000",
            "forecast_max_burst",
        ],
        &[
            "next_attack_count",
            "next_max_attack",
            "next_mean_top_attack",
        ],
        &[
            "combo_continuation_count",
            "combo_max_attack",
            "ren_length",
            "combo",
            "phase_charge",
            "phase_armed",
            "charge_pieces",
            "charge_state_seconds",
            "immediate_attack_options",
            "safe_nonfire_options",
            "max_immediate_attack",
            "accumulated_resource_gain",
            "next_i_distance",
            "next_t_distance",
            "hold_is_i_or_t",
            "ren_continuation_potential",
        ],
        &[
            "dig_delta",
            "accessible_holes",
            "dense_rows_after",
            "own_dig_burden",
        ],
        &[
            "wait_seconds",
            "wait_fraction",
            "action_seconds_delta_vs_cc",
            "timing_slack_seconds",
        ],
        &[
            "headroom_delta_vs_cc",
            "holes_delta_vs_cc",
            "covered_delta_vs_cc",
            "bumpiness_delta_vs_cc",
            "accessible_delta_vs_cc",
        ],
        &["candidate_is_alternative"],
        &["resource_balance_blocks", "own_blocks", "opponent_blocks"],
        &[
            "raw_attack",
            "sent_now",
            "pressure_after_reply",
            "baseline_raw_attack",
            "baseline_sent",
        ],
        &[
            "incoming_matured_now",
            "incoming_due_250",
            "incoming_due_500",
            "incoming_due_1000",
            "first_rise_seconds",
            "incoming_delta_observed",
            "release_trigger",
            "opponent_attack_eta",
        ],
        &[
            "fires_simultaneously",
            "synchronized_pressure",
            "initiative_seconds",
            "cancellation_dodge_gain",
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
    let offset = STRATEGY_GENOME_GROUPS.len();
    for (weight, gene) in model
        .neural_output_weights
        .iter_mut()
        .zip(&genes[offset..offset + source.neural_output_weights.len()])
    {
        *weight = *gene as f32 * 0.35;
    }
    if let Some(pc) = model
        .feature_names
        .iter()
        .position(|name| name == "perfect_clear")
    {
        model.linear_weights[pc] = 0.0;
    }
    let margin_gene = genes[offset + source.neural_output_weights.len()];
    let min_advantage =
        (config.initial_min_advantage as f64 * margin_gene.exp()).clamp(0.05, 2.0) as f32;
    (model, min_advantage)
}

fn genome_names(model: &TempoModel) -> Vec<String> {
    let mut names: Vec<_> = STRATEGY_GENOME_GROUPS
        .iter()
        .map(|name| format!("group:{name}"))
        .collect();
    names.extend((0..model.neural_output_weights.len()).map(|index| format!("mlp_output:{index}")));
    names.push("minimum_advantage".to_owned());
    names
}

fn compare_evaluation(left: &StrategyEvaluation, right: &StrategyEvaluation) -> Ordering {
    left.fitness
        .partial_cmp(&right.fitness)
        .unwrap_or(Ordering::Equal)
        .then_with(|| left.offense_ko_delta.cmp(&right.offense_ko_delta))
        .then_with(|| left.duel_score_delta.cmp(&right.duel_score_delta))
}

fn compare_checkpoint(left: &StrategyCandidate, right: &StrategyCandidate) -> Ordering {
    let left_validation = left.validation.as_ref().expect("validated checkpoint");
    let right_validation = right.validation.as_ref().expect("validated checkpoint");
    let left_robust = left.train.fitness.min(left_validation.fitness);
    let right_robust = right.train.fitness.min(right_validation.fitness);
    left_robust
        .partial_cmp(&right_robust)
        .unwrap_or(Ordering::Equal)
        .then_with(|| compare_evaluation(left_validation, right_validation))
        .then_with(|| compare_evaluation(&left.train, &right.train))
}

fn run_parallel<T: Sync, R: Send>(
    threads: usize,
    jobs: &[T],
    work: impl Fn(&T) -> Result<R> + Sync + Send,
) -> Result<Vec<R>> {
    let run = || jobs.par_iter().map(&work).collect::<Result<Vec<_>>>();
    if threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()?
            .install(run)
    } else {
        run()
    }
}

fn split_seeds(root: u64, count: usize) -> Vec<u64> {
    (0..count)
        .map(|index| split_seed(root, index as u64))
        .collect()
}

fn split_seed(seed: u64, index: u64) -> u64 {
    let mut value = seed.wrapping_add(index.wrapping_mul(0x9E37_79B9_7F4A_7C15));
    value = (value ^ (value >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    value ^ (value >> 31)
}

fn standard_normal(rng: &mut StdRng) -> f64 {
    let u1 = rng.gen::<f64>().max(f64::MIN_POSITIVE);
    let u2 = rng.gen::<f64>();
    (-2.0 * u1.ln()).sqrt() * (std::f64::consts::TAU * u2).cos()
}

fn save_parent(path: &Path) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    Ok(())
}

fn save_json(path: &Path, value: &impl Serialize) -> Result<()> {
    save_parent(path)?;
    fs::write(path, serde_json::to_vec_pretty(value)?)
        .with_context(|| format!("failed to write {}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn seed_splits_are_disjoint() {
        let train = split_seeds(1, 32);
        let validation = split_seeds(2, 32);
        let holdout = split_seeds(3, 32);
        assert!(train.iter().all(|seed| !validation.contains(seed)));
        assert!(train.iter().all(|seed| !holdout.contains(seed)));
        assert!(validation.iter().all(|seed| !holdout.contains(seed)));
    }

    #[test]
    fn strategy_genome_preserves_pc0() {
        let source = TempoModel::strategy_v2_bootstrap();
        let config = StrategyTrainingConfig::default();
        let genes = vec![0.0; STRATEGY_GENOME_GROUPS.len() + 32 + 1];
        let (model, _) = apply_strategy_genome(&source, &genes, &config);
        let pc = model
            .feature_names
            .iter()
            .position(|name| name == "perfect_clear")
            .unwrap();
        assert_eq!(model.linear_weights[pc], 0.0);
        for layer in &model.hidden_layers[..1] {
            assert!(layer.weights.iter().all(|row| row[pc] == 0.0));
        }
    }
}
