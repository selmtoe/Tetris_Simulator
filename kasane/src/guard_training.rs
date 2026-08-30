//! Paired training and evaluation for a cancellation-first KASANE defender.
//!
//! This file intentionally depends only on KASANE's public API.  Until the
//! parent crate exports this module from `lib.rs`, `src/bin/train_guard.rs`
//! includes it with a `#[path]` attribute.

use anyhow::{bail, Context, Result};
use kasane::agent::{
    AgentConfig, AgentController, AgentKind, IncomingPacket, Observation, PhaseView, PlayerView,
    SelectedAction,
};
use kasane::base::BaseModel;
use kasane::model::{BoardGeometry, TempoModel, FEATURE_NAMES};
use kasane::rules::Rules;
use kasane::search::pc0_attack;
use libtetris::Board;
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::collections::{BTreeMap, HashSet, VecDeque};
use std::fs;
use std::path::{Path, PathBuf};

pub const GUARD_GENOME_NAMES: [&str; 13] = [
    "base_prior",
    "cancel_reward",
    "rise_penalty",
    "safety_reward",
    "incoming_penalty",
    "height_penalty",
    "cavity_penalty",
    "surface_penalty",
    "attack_reward",
    "outgoing_reward",
    "time_penalty",
    "dig_reward",
    "base_guard_margin",
];

#[derive(Copy, Clone)]
struct GeneSpec {
    initial: f64,
    minimum: f64,
    maximum: f64,
}

const GENE_SPECS: [GeneSpec; GUARD_GENOME_NAMES.len()] = [
    GeneSpec {
        initial: 1.40,
        minimum: 0.30,
        maximum: 3.50,
    },
    GeneSpec {
        initial: 5.00,
        minimum: 0.00,
        maximum: 14.00,
    },
    GeneSpec {
        initial: -9.00,
        minimum: -20.00,
        maximum: -0.50,
    },
    GeneSpec {
        initial: 4.00,
        minimum: 0.25,
        maximum: 12.00,
    },
    GeneSpec {
        initial: -2.50,
        minimum: -10.00,
        maximum: 0.00,
    },
    GeneSpec {
        initial: -4.00,
        minimum: -12.00,
        maximum: -0.25,
    },
    GeneSpec {
        initial: -5.00,
        minimum: -16.00,
        maximum: -0.25,
    },
    GeneSpec {
        initial: -1.50,
        minimum: -8.00,
        maximum: 0.00,
    },
    GeneSpec {
        initial: 1.20,
        minimum: 0.00,
        maximum: 6.00,
    },
    GeneSpec {
        initial: 0.25,
        minimum: -2.00,
        maximum: 4.00,
    },
    GeneSpec {
        initial: -0.70,
        minimum: -5.00,
        maximum: 0.00,
    },
    GeneSpec {
        initial: 1.50,
        minimum: 0.00,
        maximum: 8.00,
    },
    GeneSpec {
        initial: 1.50,
        minimum: 0.00,
        maximum: 12.00,
    },
];

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct ObjectiveWeights {
    pub topout_avoidance: f64,
    pub survival_second: f64,
    pub cancelled_line: f64,
    pub risen_line: f64,
    pub mean_headroom: f64,
    pub minimum_headroom: f64,
    pub danger_second: f64,
}

impl Default for ObjectiveWeights {
    fn default() -> Self {
        Self {
            topout_avoidance: 50_000.0,
            survival_second: 500.0,
            cancelled_line: 80.0,
            risen_line: -160.0,
            mean_headroom: 300.0,
            minimum_headroom: 250.0,
            danger_second: -200.0,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct GuardTrainingConfig {
    pub population: usize,
    pub elites: usize,
    pub generations: usize,
    pub train_games: usize,
    pub validation_games: usize,
    pub final_holdout_games: usize,
    pub train_seed: u64,
    pub validation_seed: u64,
    pub final_holdout_seed: u64,
    pub threads: usize,
    pub time_limit_ms: u64,
    pub attacker_nodes: u32,
    pub defender_nodes: u32,
    pub forecast_nodes: u32,
    pub base_depth: usize,
    pub base_beam_width: usize,
    pub defender_cheese_rows: Vec<u32>,
    pub danger_headroom: i32,
    pub initial_sigma: f64,
    pub sigma_floor: f64,
    pub source_base_model: PathBuf,
    pub source_tempo_model: PathBuf,
    pub output_model: PathBuf,
    pub output_policy: PathBuf,
    pub output_report: PathBuf,
    pub objective: ObjectiveWeights,
    pub safety_gate: SafetyGateConfig,
}

impl Default for GuardTrainingConfig {
    fn default() -> Self {
        Self {
            population: 12,
            elites: 4,
            generations: 8,
            train_games: 64,
            validation_games: 64,
            final_holdout_games: 256,
            train_seed: 0x4B41_5341_4E45_4701,
            validation_seed: 0x4B41_5341_4E45_4801,
            final_holdout_seed: 0x4B41_5341_4E45_4A01,
            threads: 0,
            time_limit_ms: 20_000,
            attacker_nodes: 600,
            defender_nodes: 600,
            forecast_nodes: 200,
            base_depth: 3,
            base_beam_width: 32,
            // A stratified stress suite: all fields are survivable at t=0,
            // but incoming packets become outcome-relevant within 20 seconds.
            defender_cheese_rows: vec![10, 14, 16],
            danger_headroom: 4,
            initial_sigma: 0.18,
            sigma_floor: 0.03,
            source_base_model: PathBuf::from("models/base-model-evolved-v3.json"),
            source_tempo_model: PathBuf::from("config/tempo-model-bootstrap-v3.json"),
            output_model: PathBuf::from("models/guard-model-evolved-v1.json"),
            output_policy: PathBuf::from("config/kasane-guard-v1.json"),
            output_report: PathBuf::from("results/guard-training-v1.json"),
            objective: ObjectiveWeights::default(),
            safety_gate: SafetyGateConfig::default(),
        }
    }
}

impl GuardTrainingConfig {
    pub fn validate(&self) -> Result<()> {
        if self.population < 2 {
            bail!("population must be at least two");
        }
        if self.elites == 0 || self.elites > self.population {
            bail!("elites must be in 1..=population");
        }
        if self.generations == 0
            || self.train_games == 0
            || self.validation_games == 0
            || self.final_holdout_games == 0
        {
            bail!("generations and every split size must be positive");
        }
        if self.train_seed == self.validation_seed
            || self.train_seed == self.final_holdout_seed
            || self.validation_seed == self.final_holdout_seed
        {
            bail!("train, validation and final_holdout roots must all differ");
        }
        if self.time_limit_ms == 0 {
            bail!("time_limit_ms must be positive");
        }
        if self.attacker_nodes == 0
            || self.defender_nodes == 0
            || self.forecast_nodes == 0
            || self.base_depth == 0
            || self.base_beam_width == 0
        {
            bail!("search budgets and base search dimensions must be positive");
        }
        if self.defender_cheese_rows.is_empty()
            || self.defender_cheese_rows.iter().any(|rows| *rows > 19)
        {
            bail!("defender_cheese_rows must contain values in 0..=19");
        }
        if !(0..20).contains(&self.danger_headroom) {
            bail!("danger_headroom must be in 0..20");
        }
        if !self.initial_sigma.is_finite()
            || self.initial_sigma <= 0.0
            || !self.sigma_floor.is_finite()
            || self.sigma_floor <= 0.0
        {
            bail!("sigma values must be finite and positive");
        }
        Rules::pinned().validate()?;
        self.safety_gate.validate()?;
        let train: HashSet<_> = trial_seeds(self.train_seed, self.train_games)
            .into_iter()
            .collect();
        let validation: HashSet<_> = trial_seeds(self.validation_seed, self.validation_games)
            .into_iter()
            .collect();
        let final_holdout: HashSet<_> =
            trial_seeds(self.final_holdout_seed, self.final_holdout_games)
                .into_iter()
                .collect();
        if !train.is_disjoint(&validation)
            || !train.is_disjoint(&final_holdout)
            || !validation.is_disjoint(&final_holdout)
        {
            bail!("derived train, validation and final_holdout seeds overlap");
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct SafetyGateConfig {
    /// A tactical action may not lose this many projected empty rows versus
    /// the Cold Clear fallback.
    pub max_headroom_loss: i32,
    pub max_hole_increase: u32,
    pub max_covered_increase: u32,
    pub max_bumpiness_increase: u32,
    pub max_accessible_hole_loss: u32,
    /// With equal geometry, require this much extra immediate cancellation.
    pub minimum_cancel_gain: u32,
}

impl Default for SafetyGateConfig {
    fn default() -> Self {
        Self {
            max_headroom_loss: 0,
            max_hole_increase: 0,
            max_covered_increase: 0,
            max_bumpiness_increase: 2,
            max_accessible_hole_loss: 0,
            minimum_cancel_gain: 1,
        }
    }
}

impl SafetyGateConfig {
    fn validate(&self) -> Result<()> {
        if !(0..=4).contains(&self.max_headroom_loss) {
            bail!("safety_gate.max_headroom_loss must be in 0..=4");
        }
        if self.max_hole_increase > 8
            || self.max_covered_increase > 12
            || self.max_bumpiness_increase > 20
            || self.max_accessible_hole_loss > 8
        {
            bail!("safety gate geometry tolerances are too large");
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DefenseMetrics {
    pub topout: bool,
    pub survival_ms: u64,
    pub pieces: u64,
    pub raw_attack: u64,
    pub cancelled: u64,
    pub sent: u64,
    pub received: u64,
    /// Garbage lines whose grace expired, including the lethal line and any
    /// remaining lines in the same matured packet.
    pub rise_attempted: u64,
    pub risen: u64,
    pub pending_at_end: u64,
    pub minimum_headroom: i32,
    pub mean_headroom: f64,
    pub maximum_height: u32,
    pub danger_time_ms: u64,
    pub gate_considered: u64,
    pub gate_accepted: u64,
    pub gate_fallbacks: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct AttackScheduleSummary {
    pub packets: u64,
    pub lines: u64,
    pub digest: String,
    pub source_exhausted: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DefenseTrial {
    pub seed: u64,
    pub defender_cheese_rows: u32,
    pub defense: DefenseMetrics,
    pub attack: AttackScheduleSummary,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DefenseSummary {
    pub games: usize,
    pub topouts: usize,
    pub topout_rate: f64,
    pub topout_wilson_low: f64,
    pub topout_wilson_high: f64,
    pub mean_survival_ms: f64,
    pub mean_cancelled: f64,
    pub cancellation_rate: f64,
    pub mean_rise_attempted: f64,
    /// Cancelled / (cancelled + rise_attempted); pending packets are excluded.
    pub garbage_rise_avoidance_rate: f64,
    pub mean_risen: f64,
    pub rise_rate: f64,
    pub mean_minimum_headroom: f64,
    pub mean_headroom: f64,
    pub mean_danger_time_ms: f64,
    pub mean_attack_lines: f64,
    pub gate_acceptance_rate: f64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PairedDelta {
    /// Positive means Guard topouts less often than Cold Clear.
    pub topout_reduction_percentage_points: f64,
    pub survival_gain_ms: f64,
    pub cancelled_gain: f64,
    /// Negative means fewer garbage lines reached their rise event on Guard.
    pub rise_attempted_delta: f64,
    /// Negative means fewer garbage lines were inserted on Guard.
    pub risen_delta: f64,
    pub mean_headroom_gain: f64,
    pub minimum_headroom_gain: f64,
    /// Negative means Guard spent less time near the ceiling.
    pub danger_time_delta_ms: f64,
    pub guard_only_survivals: usize,
    pub cold_clear_only_survivals: usize,
    pub survival_difference_ci95_low_pp: f64,
    pub survival_difference_ci95_high_pp: f64,
    pub objective_fitness: f64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PairedEvaluation {
    pub games: usize,
    pub attack_schedules_identical: bool,
    pub cold_clear: DefenseSummary,
    pub guard: DefenseSummary,
    pub paired: PairedDelta,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct GuardCandidate {
    pub index: usize,
    pub genes: BTreeMap<String, f64>,
    pub base_guard_margin: f32,
    pub train: PairedEvaluation,
    pub validation: PairedEvaluation,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct GuardGeneration {
    pub generation: usize,
    pub mean: BTreeMap<String, f64>,
    pub sigma: BTreeMap<String, f64>,
    pub candidates: Vec<GuardCandidate>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct GuardPolicyArtifact {
    pub schema: String,
    pub base_model: PathBuf,
    pub tempo_model: PathBuf,
    pub agent_kind: AgentKind,
    pub strict_base_policy: bool,
    pub enable_tempo: bool,
    pub enable_tank: bool,
    pub enable_charge: bool,
    pub maximum_wait_ms: u64,
    pub base_guard_margin: f32,
    pub base_depth: usize,
    pub base_beam_width: usize,
    pub forecast_nodes: u32,
    pub safety_fallback: AgentKind,
    pub safety_gate: SafetyGateConfig,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct GuardTrainingReport {
    pub schema: String,
    pub rules: Rules,
    pub attack_contract: String,
    pub config: GuardTrainingConfig,
    pub genome_names: Vec<String>,
    pub generations: Vec<GuardGeneration>,
    pub best_genes: BTreeMap<String, f64>,
    pub best_base_guard_margin: f32,
    pub selected_generation: usize,
    pub train: PairedEvaluation,
    pub validation: PairedEvaluation,
    pub final_holdout: PairedEvaluation,
    pub final_holdout_improves_topout_rate: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct GuardEvaluationConfig {
    pub games: usize,
    pub seed: u64,
    pub threads: usize,
    pub time_limit_ms: u64,
    pub attacker_nodes: u32,
    pub defender_nodes: u32,
    pub forecast_nodes: u32,
    pub base_depth: usize,
    pub base_beam_width: usize,
    pub defender_cheese_rows: Vec<u32>,
    pub danger_headroom: i32,
    pub policy_path: PathBuf,
    pub output_report: PathBuf,
    pub objective: ObjectiveWeights,
}

impl Default for GuardEvaluationConfig {
    fn default() -> Self {
        Self {
            games: 256,
            seed: 0x4B41_5341_4E45_5001,
            threads: 0,
            time_limit_ms: 20_000,
            attacker_nodes: 600,
            defender_nodes: 600,
            forecast_nodes: 200,
            base_depth: 3,
            base_beam_width: 32,
            defender_cheese_rows: vec![10, 14, 16],
            danger_headroom: 4,
            policy_path: PathBuf::from("config/kasane-guard-v1.json"),
            output_report: PathBuf::from("results/guard-holdout-strong-v1.json"),
            objective: ObjectiveWeights::default(),
        }
    }
}

impl GuardEvaluationConfig {
    pub fn validate(&self) -> Result<()> {
        if self.games == 0 || self.time_limit_ms == 0 {
            bail!("games and time_limit_ms must be positive");
        }
        if self.attacker_nodes == 0
            || self.defender_nodes == 0
            || self.forecast_nodes == 0
            || self.base_depth == 0
            || self.base_beam_width == 0
        {
            bail!("search budgets and base search dimensions must be positive");
        }
        if self.defender_cheese_rows.is_empty()
            || self.defender_cheese_rows.iter().any(|rows| *rows > 19)
        {
            bail!("defender_cheese_rows must contain values in 0..=19");
        }
        if !(0..20).contains(&self.danger_headroom) {
            bail!("danger_headroom must be in 0..20");
        }
        Rules::pinned().validate()?;
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct GuardEvaluationReport {
    pub schema: String,
    pub rules: Rules,
    pub attack_contract: String,
    pub config: GuardEvaluationConfig,
    pub policy: GuardPolicyArtifact,
    pub evaluation: PairedEvaluation,
    pub improves_topout_rate: bool,
}

#[derive(Clone)]
struct GuardPolicy {
    model: TempoModel,
    base_guard_margin: f32,
    safety_gate: SafetyGateConfig,
}

#[derive(Clone)]
struct GenomeSample {
    index: usize,
    genes: Vec<f64>,
    policy: GuardPolicy,
}

/// Train on one seed root, select a generation checkpoint on a second root,
/// and evaluate the frozen checkpoint once on a third root.
pub fn run_guard_training(config: GuardTrainingConfig) -> Result<GuardTrainingReport> {
    config.validate()?;
    let base_model = BaseModel::load(&config.source_base_model)?;
    let tempo_source = TempoModel::load(&config.source_tempo_model)?;
    let train_seeds = trial_seeds(config.train_seed, config.train_games);
    let validation_seeds = trial_seeds(config.validation_seed, config.validation_games);
    let final_holdout_seeds = trial_seeds(config.final_holdout_seed, config.final_holdout_games);

    eprintln!("evaluating paired Cold Clear train baseline...");
    let train_baseline = evaluate_trials(Defender::ColdClear, &base_model, &config, &train_seeds)?;
    eprintln!("evaluating paired Cold Clear validation baseline...");
    let validation_baseline =
        evaluate_trials(Defender::ColdClear, &base_model, &config, &validation_seeds)?;

    let mut rng = StdRng::seed_from_u64(split_seed(config.train_seed, 0xC3E0));
    let mut mean: Vec<f64> = GENE_SPECS.iter().map(|spec| spec.initial).collect();
    let mut sigma: Vec<f64> = GENE_SPECS
        .iter()
        .map(|spec| (spec.maximum - spec.minimum) * config.initial_sigma)
        .collect();
    let mut generations = Vec::with_capacity(config.generations);
    let mut global_best: Option<(usize, GuardCandidate, GuardPolicy, Vec<f64>)> = None;

    for generation in 0..config.generations {
        let mut samples = Vec::with_capacity(config.population);
        for index in 0..config.population {
            let genes = if generation == 0 {
                seeded_genome(index).unwrap_or_else(|| sample_genome(&mean, &sigma, &mut rng))
            } else if index == 0 {
                mean.clone()
            } else {
                sample_genome(&mean, &sigma, &mut rng)
            };
            let policy = policy_from_genome(&tempo_source, &genes, &config.safety_gate)?;
            samples.push(GenomeSample {
                index,
                genes,
                policy,
            });
        }

        let train_evaluations = evaluate_population(
            &samples,
            &train_baseline,
            &base_model,
            &config,
            &train_seeds,
        )?;
        let validation_evaluations = evaluate_population(
            &samples,
            &validation_baseline,
            &base_model,
            &config,
            &validation_seeds,
        )?;
        let candidates: Vec<_> = samples
            .iter()
            .zip(train_evaluations)
            .zip(validation_evaluations)
            .map(|((sample, train), validation)| GuardCandidate {
                index: sample.index,
                genes: genes_as_map(&sample.genes),
                base_guard_margin: sample.policy.base_guard_margin,
                train,
                validation,
            })
            .collect();
        let mut train_ranked = candidates.clone();
        train_ranked.sort_by(compare_train_candidates);
        let validation_best = candidates
            .iter()
            .min_by(|left, right| compare_validation_candidates(left, right))
            .expect("population is non-empty")
            .clone();
        let best_sample = samples
            .iter()
            .find(|sample| sample.index == validation_best.index)
            .expect("candidate must have a source sample");
        let replace_global = global_best
            .as_ref()
            .map(|(_, current, _, _)| {
                compare_validation_candidates(&validation_best, current) == Ordering::Less
            })
            .unwrap_or(true);
        if replace_global {
            global_best = Some((
                generation,
                validation_best.clone(),
                best_sample.policy.clone(),
                best_sample.genes.clone(),
            ));
        }

        // Evolution sees train only. Validation chooses the checkpoint but
        // never updates the CEM distribution, limiting validation overfit.
        let elite_genes: Vec<_> = train_ranked[..config.elites]
            .iter()
            .map(|candidate| {
                GUARD_GENOME_NAMES
                    .iter()
                    .map(|name| candidate.genes[*name])
                    .collect::<Vec<_>>()
            })
            .collect();
        for dimension in 0..GUARD_GENOME_NAMES.len() {
            let elite_mean = elite_genes
                .iter()
                .map(|genes| genes[dimension])
                .sum::<f64>()
                / elite_genes.len() as f64;
            let variance = elite_genes
                .iter()
                .map(|genes| (genes[dimension] - elite_mean).powi(2))
                .sum::<f64>()
                / elite_genes.len() as f64;
            mean[dimension] = 0.25 * mean[dimension] + 0.75 * elite_mean;
            let range = GENE_SPECS[dimension].maximum - GENE_SPECS[dimension].minimum;
            let floor = range * config.sigma_floor;
            sigma[dimension] =
                (0.30 * sigma[dimension] + 0.70 * variance.sqrt().max(floor)).max(floor);
        }

        generations.push(GuardGeneration {
            generation,
            mean: genes_as_map(&mean),
            sigma: genes_as_map(&sigma),
            candidates: train_ranked,
        });
        let (_, global, policy, _) = global_best.as_ref().expect("one generation exists");
        save_model(&config.output_model, &policy.model)?;
        eprintln!(
            "guard_generation={} train_topouts={}/{} validation_topouts={}/{} validation_cc={} selected_validation_topouts={} fitness={:.3}",
            generation,
            validation_best.train.guard.topouts,
            config.train_games,
            validation_best.validation.guard.topouts,
            config.validation_games,
            validation_best.validation.cold_clear.topouts,
            global.validation.guard.topouts,
            global.validation.paired.objective_fitness,
        );
    }

    let (selected_generation, _, best_policy, best_gene_values) =
        global_best.expect("at least one generation");
    eprintln!("re-evaluating selected checkpoint on train seeds...");
    let train_guard = evaluate_trials(
        Defender::Guard(best_policy.clone()),
        &base_model,
        &config,
        &train_seeds,
    )?;
    let train = compare_trials(&train_baseline, &train_guard, &config.objective)?;

    eprintln!("re-evaluating selected checkpoint on validation seeds...");
    let validation_guard = evaluate_trials(
        Defender::Guard(best_policy.clone()),
        &base_model,
        &config,
        &validation_seeds,
    )?;
    let validation = compare_trials(&validation_baseline, &validation_guard, &config.objective)?;

    eprintln!("evaluating frozen checkpoint on untouched final_holdout seeds...");
    let final_holdout_baseline = evaluate_trials(
        Defender::ColdClear,
        &base_model,
        &config,
        &final_holdout_seeds,
    )?;
    let final_holdout_guard = evaluate_trials(
        Defender::Guard(best_policy.clone()),
        &base_model,
        &config,
        &final_holdout_seeds,
    )?;
    let final_holdout = compare_trials(
        &final_holdout_baseline,
        &final_holdout_guard,
        &config.objective,
    )?;

    save_model(&config.output_model, &best_policy.model)?;
    save_policy(&config, best_policy.base_guard_margin)?;
    let report = GuardTrainingReport {
        schema: "kasane-guard-training/v2-three-way-split".to_owned(),
        rules: Rules::pinned(),
        attack_contract: "Cold Clear pressure source is isolated from defender outgoing attack; paired runs share exact piece, garbage-hole and timestamped attack streams, verified by a full-horizon digest; Guard deviations pass a projected-safety gate against the Cold Clear fallback; PC special attack/reward is zero."
            .to_owned(),
        config: config.clone(),
        genome_names: GUARD_GENOME_NAMES.iter().map(|name| (*name).to_owned()).collect(),
        generations,
        best_genes: genes_as_map(&best_gene_values),
        best_base_guard_margin: best_policy.base_guard_margin,
        selected_generation,
        train,
        validation,
        final_holdout_improves_topout_rate: final_holdout.guard.topout_rate
            < final_holdout.cold_clear.topout_rate,
        final_holdout,
    };
    save_json(&config.output_report, &report)?;
    Ok(report)
}

/// Evaluate an already-trained Guard policy without sampling, mutating or
/// saving model weights.  The seed root is independent from training splits.
pub fn run_guard_evaluation(config: GuardEvaluationConfig) -> Result<GuardEvaluationReport> {
    config.validate()?;
    let policy_bytes = fs::read(&config.policy_path)
        .with_context(|| format!("failed to read {}", config.policy_path.display()))?;
    let policy_artifact: GuardPolicyArtifact = serde_json::from_slice(&policy_bytes)
        .with_context(|| format!("failed to parse {}", config.policy_path.display()))?;
    if policy_artifact.agent_kind != AgentKind::Kasane
        || policy_artifact.safety_fallback != AgentKind::ColdClear
        || policy_artifact.strict_base_policy
        || !policy_artifact.enable_tempo
        || policy_artifact.enable_tank
        || policy_artifact.enable_charge
        || policy_artifact.maximum_wait_ms != 0
    {
        bail!("policy is not a cancellation-first KASANE Guard artifact");
    }
    policy_artifact.safety_gate.validate()?;
    let base_model = BaseModel::load(&policy_artifact.base_model)?;
    let tempo_model = TempoModel::load(&policy_artifact.tempo_model)?;
    let policy = GuardPolicy {
        model: tempo_model,
        base_guard_margin: policy_artifact.base_guard_margin,
        safety_gate: policy_artifact.safety_gate.clone(),
    };
    let runtime_config = GuardTrainingConfig {
        threads: config.threads,
        time_limit_ms: config.time_limit_ms,
        attacker_nodes: config.attacker_nodes,
        defender_nodes: config.defender_nodes,
        forecast_nodes: config.forecast_nodes,
        base_depth: config.base_depth,
        base_beam_width: config.base_beam_width,
        defender_cheese_rows: config.defender_cheese_rows.clone(),
        danger_headroom: config.danger_headroom,
        safety_gate: policy_artifact.safety_gate.clone(),
        objective: config.objective.clone(),
        ..GuardTrainingConfig::default()
    };
    let seeds = trial_seeds(config.seed, config.games);
    eprintln!("evaluate-only: running paired Cold Clear baseline...");
    let baseline = evaluate_trials(Defender::ColdClear, &base_model, &runtime_config, &seeds)?;
    eprintln!("evaluate-only: running frozen KASANE Guard policy...");
    let guard = evaluate_trials(
        Defender::Guard(policy),
        &base_model,
        &runtime_config,
        &seeds,
    )?;
    let evaluation = compare_trials(&baseline, &guard, &config.objective)?;
    let report = GuardEvaluationReport {
        schema: "kasane-guard-evaluation/v1-frozen-policy".to_owned(),
        rules: Rules::pinned(),
        attack_contract: "Frozen policy; no mutation or training. Cold Clear pressure source is isolated from defender outgoing attack; every pair verifies an identical full-horizon attack digest; PC special attack/reward is zero."
            .to_owned(),
        config: config.clone(),
        policy: policy_artifact,
        improves_topout_rate: evaluation.guard.topout_rate
            < evaluation.cold_clear.topout_rate,
        evaluation,
    };
    save_json(&config.output_report, &report)?;
    Ok(report)
}

fn compare_train_candidates(left: &GuardCandidate, right: &GuardCandidate) -> Ordering {
    compare_evaluations(&left.train, &right.train).then_with(|| left.index.cmp(&right.index))
}

fn compare_validation_candidates(left: &GuardCandidate, right: &GuardCandidate) -> Ordering {
    compare_evaluations(&left.validation, &right.validation)
        .then_with(|| compare_evaluations(&left.train, &right.train))
        .then_with(|| left.index.cmp(&right.index))
}

fn compare_evaluations(left: &PairedEvaluation, right: &PairedEvaluation) -> Ordering {
    left.guard
        .topouts
        .cmp(&right.guard.topouts)
        .then_with(|| {
            right
                .paired
                .objective_fitness
                .partial_cmp(&left.paired.objective_fitness)
                .unwrap_or(Ordering::Equal)
        })
        .then_with(|| {
            right
                .guard
                .mean_survival_ms
                .partial_cmp(&left.guard.mean_survival_ms)
                .unwrap_or(Ordering::Equal)
        })
        .then_with(|| {
            right
                .guard
                .mean_headroom
                .partial_cmp(&left.guard.mean_headroom)
                .unwrap_or(Ordering::Equal)
        })
}

fn evaluate_population(
    samples: &[GenomeSample],
    baseline: &[DefenseTrial],
    base_model: &BaseModel,
    config: &GuardTrainingConfig,
    seeds: &[u64],
) -> Result<Vec<PairedEvaluation>> {
    let jobs = samples.len() * seeds.len();
    let run = || {
        (0..jobs)
            .into_par_iter()
            .map(|job| {
                let candidate = job / seeds.len();
                let game = job % seeds.len();
                let rows = scenario_rows(config, game);
                let trial = run_fixed_pressure_trial(
                    seeds[game],
                    rows,
                    Defender::Guard(samples[candidate].policy.clone()),
                    base_model,
                    config,
                )?;
                Ok((candidate, trial))
            })
            .collect::<Result<Vec<_>>>()
    };
    let results = install_pool(config.threads, run)?;
    let mut grouped: Vec<Vec<DefenseTrial>> = (0..samples.len())
        .map(|_| Vec::with_capacity(seeds.len()))
        .collect();
    for (candidate, trial) in results {
        grouped[candidate].push(trial);
    }
    for trials in &mut grouped {
        trials.sort_by_key(|trial| trial.seed);
    }
    let mut baseline_sorted = baseline.to_vec();
    baseline_sorted.sort_by_key(|trial| trial.seed);
    grouped
        .iter()
        .map(|trials| compare_trials(&baseline_sorted, trials, &config.objective))
        .collect()
}

#[derive(Clone)]
enum Defender {
    ColdClear,
    Guard(GuardPolicy),
}

fn evaluate_trials(
    defender: Defender,
    base_model: &BaseModel,
    config: &GuardTrainingConfig,
    seeds: &[u64],
) -> Result<Vec<DefenseTrial>> {
    let run = || {
        (0..seeds.len())
            .into_par_iter()
            .map(|index| {
                run_fixed_pressure_trial(
                    seeds[index],
                    scenario_rows(config, index),
                    defender.clone(),
                    base_model,
                    config,
                )
            })
            .collect::<Result<Vec<_>>>()
    };
    let mut trials = install_pool(config.threads, run)?;
    trials.sort_by_key(|trial| trial.seed);
    Ok(trials)
}

fn install_pool<T: Send>(threads: usize, run: impl FnOnce() -> Result<T> + Send) -> Result<T> {
    if threads == 0 {
        run()
    } else {
        rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()?
            .install(run)
    }
}

fn compare_trials(
    baseline: &[DefenseTrial],
    guard: &[DefenseTrial],
    objective: &ObjectiveWeights,
) -> Result<PairedEvaluation> {
    if baseline.len() != guard.len() || baseline.is_empty() {
        bail!("paired evaluation requires equally sized, non-empty trial sets");
    }
    let mut values = Vec::with_capacity(baseline.len());
    let mut fitness = 0.0;
    let mut guard_only_survivals = 0;
    let mut cold_clear_only_survivals = 0;
    for (cold_clear, candidate) in baseline.iter().zip(guard) {
        if cold_clear.seed != candidate.seed
            || cold_clear.defender_cheese_rows != candidate.defender_cheese_rows
        {
            bail!("paired trials do not share seed and initial field");
        }
        if cold_clear.attack.digest != candidate.attack.digest
            || cold_clear.attack.packets != candidate.attack.packets
            || cold_clear.attack.lines != candidate.attack.lines
        {
            bail!(
                "fixed attack schedule diverged for seed {} ({} != {})",
                cold_clear.seed,
                cold_clear.attack.digest,
                candidate.attack.digest
            );
        }
        let cold_survived = !cold_clear.defense.topout;
        let guard_survived = !candidate.defense.topout;
        values.push(guard_survived as u8 as f64 - cold_survived as u8 as f64);
        guard_only_survivals += (guard_survived && !cold_survived) as usize;
        cold_clear_only_survivals += (cold_survived && !guard_survived) as usize;
        fitness += pair_objective(&cold_clear.defense, &candidate.defense, objective);
    }
    let count = baseline.len() as f64;
    let mean = values.iter().sum::<f64>() / count;
    let variance = if values.len() > 1 {
        values
            .iter()
            .map(|value| (value - mean).powi(2))
            .sum::<f64>()
            / (count - 1.0)
    } else {
        0.0
    };
    let half_width = 1.96 * (variance / count).sqrt();
    let cold_summary = summarize_trials(baseline);
    let guard_summary = summarize_trials(guard);
    Ok(PairedEvaluation {
        games: baseline.len(),
        attack_schedules_identical: true,
        paired: PairedDelta {
            topout_reduction_percentage_points: (cold_summary.topout_rate
                - guard_summary.topout_rate)
                * 100.0,
            survival_gain_ms: guard_summary.mean_survival_ms - cold_summary.mean_survival_ms,
            cancelled_gain: guard_summary.mean_cancelled - cold_summary.mean_cancelled,
            rise_attempted_delta: guard_summary.mean_rise_attempted
                - cold_summary.mean_rise_attempted,
            risen_delta: guard_summary.mean_risen - cold_summary.mean_risen,
            mean_headroom_gain: guard_summary.mean_headroom - cold_summary.mean_headroom,
            minimum_headroom_gain: guard_summary.mean_minimum_headroom
                - cold_summary.mean_minimum_headroom,
            danger_time_delta_ms: guard_summary.mean_danger_time_ms
                - cold_summary.mean_danger_time_ms,
            guard_only_survivals,
            cold_clear_only_survivals,
            survival_difference_ci95_low_pp: (mean - half_width) * 100.0,
            survival_difference_ci95_high_pp: (mean + half_width) * 100.0,
            objective_fitness: fitness / count,
        },
        cold_clear: cold_summary,
        guard: guard_summary,
    })
}

fn pair_objective(
    cold_clear: &DefenseMetrics,
    guard: &DefenseMetrics,
    weights: &ObjectiveWeights,
) -> f64 {
    let survival_indicator = (!guard.topout) as u8 as f64 - (!cold_clear.topout) as u8 as f64;
    survival_indicator * weights.topout_avoidance
        + (guard.survival_ms as f64 - cold_clear.survival_ms as f64) / 1000.0
            * weights.survival_second
        + (guard.cancelled as f64 - cold_clear.cancelled as f64) * weights.cancelled_line
        + (guard.rise_attempted as f64 - cold_clear.rise_attempted as f64) * weights.risen_line
        + (guard.mean_headroom - cold_clear.mean_headroom) * weights.mean_headroom
        + (guard.minimum_headroom - cold_clear.minimum_headroom) as f64 * weights.minimum_headroom
        + (guard.danger_time_ms as f64 - cold_clear.danger_time_ms as f64) / 1000.0
            * weights.danger_second
}

fn summarize_trials(trials: &[DefenseTrial]) -> DefenseSummary {
    let games = trials.len();
    let n = games as f64;
    let topouts = trials.iter().filter(|trial| trial.defense.topout).count();
    let received: u64 = trials.iter().map(|trial| trial.defense.received).sum();
    let cancelled: u64 = trials.iter().map(|trial| trial.defense.cancelled).sum();
    let rise_attempted: u64 = trials
        .iter()
        .map(|trial| trial.defense.rise_attempted)
        .sum();
    let risen: u64 = trials.iter().map(|trial| trial.defense.risen).sum();
    let gate_considered: u64 = trials
        .iter()
        .map(|trial| trial.defense.gate_considered)
        .sum();
    let gate_accepted: u64 = trials.iter().map(|trial| trial.defense.gate_accepted).sum();
    let (wilson_low, wilson_high) = wilson(topouts, games);
    DefenseSummary {
        games,
        topouts,
        topout_rate: topouts as f64 / n,
        topout_wilson_low: wilson_low,
        topout_wilson_high: wilson_high,
        mean_survival_ms: mean(trials, |trial| trial.defense.survival_ms as f64),
        mean_cancelled: cancelled as f64 / n,
        cancellation_rate: if received == 0 {
            0.0
        } else {
            cancelled as f64 / received as f64
        },
        mean_rise_attempted: rise_attempted as f64 / n,
        garbage_rise_avoidance_rate: if cancelled + rise_attempted == 0 {
            0.0
        } else {
            cancelled as f64 / (cancelled + rise_attempted) as f64
        },
        mean_risen: risen as f64 / n,
        rise_rate: if received == 0 {
            0.0
        } else {
            risen as f64 / received as f64
        },
        mean_minimum_headroom: mean(trials, |trial| trial.defense.minimum_headroom as f64),
        mean_headroom: mean(trials, |trial| trial.defense.mean_headroom),
        mean_danger_time_ms: mean(trials, |trial| trial.defense.danger_time_ms as f64),
        mean_attack_lines: mean(trials, |trial| trial.attack.lines as f64),
        gate_acceptance_rate: if gate_considered == 0 {
            0.0
        } else {
            gate_accepted as f64 / gate_considered as f64
        },
    }
}

fn mean(trials: &[DefenseTrial], value: impl Fn(&DefenseTrial) -> f64) -> f64 {
    trials.iter().map(value).sum::<f64>() / trials.len() as f64
}

fn wilson(successes: usize, total: usize) -> (f64, f64) {
    if total == 0 {
        return (0.0, 0.0);
    }
    let z = 1.96;
    let n = total as f64;
    let p = successes as f64 / n;
    let denominator = 1.0 + z * z / n;
    let center = (p + z * z / (2.0 * n)) / denominator;
    let radius = z * ((p * (1.0 - p) / n + z * z / (4.0 * n * n)).sqrt()) / denominator;
    ((center - radius).max(0.0), (center + radius).min(1.0))
}

fn policy_from_genome(
    source: &TempoModel,
    genes: &[f64],
    safety_gate: &SafetyGateConfig,
) -> Result<GuardPolicy> {
    if genes.len() != GUARD_GENOME_NAMES.len() {
        bail!("guard genome has the wrong width");
    }
    let mut model = source.clone();
    model.linear_weights.fill(0.0);
    model.linear_bias = 0.0;
    model.hidden_layers.clear();
    model.neural_output_weights.clear();
    model.neural_output_bias = 0.0;

    set_weight(&mut model, "base_value", genes[0] as f32)?;
    set_weight(&mut model, "base_spike", genes[0] as f32 * 0.20)?;
    set_weight(&mut model, "cancelled_own", genes[1] as f32)?;
    set_weight(&mut model, "rise_after_action", genes[2] as f32)?;
    set_weight(&mut model, "safety_margin", genes[3] as f32)?;
    set_weight(&mut model, "incoming_now", genes[4] as f32 * 0.50)?;
    set_weight(&mut model, "incoming_at_lock", genes[4] as f32)?;
    set_weight(&mut model, "own_height", genes[5] as f32)?;
    set_weight(&mut model, "own_holes", genes[6] as f32)?;
    set_weight(&mut model, "own_covered", genes[6] as f32 * 0.45)?;
    set_weight(&mut model, "own_bumpiness", genes[7] as f32)?;
    set_weight(&mut model, "raw_attack", genes[8] as f32)?;
    set_weight(&mut model, "sent_now", genes[9] as f32)?;
    set_weight(&mut model, "pressure_after_reply", genes[9] as f32 * 0.25)?;
    set_weight(&mut model, "action_seconds", genes[10] as f32)?;
    set_weight(&mut model, "wait_seconds", genes[10] as f32 * 1.50)?;
    set_weight(&mut model, "clear_delay_seconds", genes[10] as f32)?;
    set_weight(&mut model, "dig_delta", genes[11] as f32)?;
    set_weight(&mut model, "accessible_holes", genes[11] as f32 * 0.35)?;
    set_weight(&mut model, "perfect_clear", 0.0)?;
    model.validate()?;
    Ok(GuardPolicy {
        model,
        base_guard_margin: genes[12] as f32,
        safety_gate: safety_gate.clone(),
    })
}

fn set_weight(model: &mut TempoModel, name: &str, value: f32) -> Result<()> {
    let index = FEATURE_NAMES
        .iter()
        .position(|candidate| *candidate == name)
        .with_context(|| format!("Tempo feature {name} is unavailable"))?;
    model.linear_weights[index] = value;
    Ok(())
}

fn genes_as_map(genes: &[f64]) -> BTreeMap<String, f64> {
    GUARD_GENOME_NAMES
        .iter()
        .zip(genes)
        .map(|(name, value)| ((*name).to_owned(), *value))
        .collect()
}

fn scenario_rows(config: &GuardTrainingConfig, index: usize) -> u32 {
    config.defender_cheese_rows[index % config.defender_cheese_rows.len()]
}

fn save_model(path: &Path, model: &TempoModel) -> Result<()> {
    ensure_parent(path)?;
    model
        .save(path)
        .with_context(|| format!("failed to save {}", path.display()))
}

fn save_policy(config: &GuardTrainingConfig, base_guard_margin: f32) -> Result<()> {
    let artifact = GuardPolicyArtifact {
        schema: "kasane-guard-policy/v1".to_owned(),
        base_model: config.source_base_model.clone(),
        tempo_model: config.output_model.clone(),
        agent_kind: AgentKind::Kasane,
        strict_base_policy: false,
        enable_tempo: true,
        enable_tank: false,
        enable_charge: false,
        maximum_wait_ms: 0,
        base_guard_margin,
        base_depth: config.base_depth,
        base_beam_width: config.base_beam_width,
        forecast_nodes: config.forecast_nodes,
        safety_fallback: AgentKind::ColdClear,
        safety_gate: config.safety_gate.clone(),
    };
    save_json(&config.output_policy, &artifact)
}

fn save_json(path: &Path, value: &impl Serialize) -> Result<()> {
    ensure_parent(path)?;
    fs::write(path, serde_json::to_vec_pretty(value)?)
        .with_context(|| format!("failed to write {}", path.display()))
}

fn ensure_parent(path: &Path) -> Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)
                .with_context(|| format!("failed to create {}", parent.display()))?;
        }
    }
    Ok(())
}

fn standard_normal(rng: &mut StdRng) -> f64 {
    let u1 = rng.gen::<f64>().max(f64::MIN_POSITIVE);
    let u2 = rng.gen::<f64>();
    (-2.0 * u1.ln()).sqrt() * (std::f64::consts::TAU * u2).cos()
}

fn sample_genome(mean: &[f64], sigma: &[f64], rng: &mut StdRng) -> Vec<f64> {
    mean.iter()
        .zip(sigma)
        .zip(GENE_SPECS)
        .map(|((&center, &spread), spec)| {
            (center + spread * standard_normal(rng)).clamp(spec.minimum, spec.maximum)
        })
        .collect()
}

fn seeded_genome(index: usize) -> Option<Vec<f64>> {
    let values: &[f64] = match index {
        0 => return Some(GENE_SPECS.iter().map(|spec| spec.initial).collect()),
        // Conservative anchor: preserve the independent Base choice unless a
        // tactical move has a large, visible survival advantage.
        1 => &[
            2.80, 2.00, -14.0, 8.00, -4.00, -8.00, -10.0, -3.00, 1.00, 0.00, -1.50, 3.00, 10.0,
        ],
        // Balanced cancellation anchor.
        2 => &[
            1.80, 8.00, -14.0, 7.00, -3.00, -6.00, -8.00, -2.00, 2.00, 0.10, -1.20, 2.00, 4.00,
        ],
        // Dig-and-headroom anchor for high strict-cheese starts.
        3 => &[
            2.20, 4.00, -12.0, 7.00, -3.00, -7.00, -9.00, -2.00, 1.00, 0.00, -1.50, 6.00, 6.00,
        ],
        _ => return None,
    };
    Some(values.to_vec())
}

fn trial_seeds(root: u64, count: usize) -> Vec<u64> {
    (0..count)
        .map(|index| split_seed(root, index as u64))
        .collect()
}

fn split_seed(seed: u64, stream: u64) -> u64 {
    let mut value = seed ^ stream.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    value = (value ^ (value >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    value ^ (value >> 31)
}

#[derive(Clone, Debug)]
struct GarbagePacket {
    lines: u32,
    arrival_ms: u64,
}

enum Phase {
    Ready,
    Moving {
        started_ms: u64,
        lock_ms: u64,
        selected: SelectedAction,
    },
    LineClear {
        ends_ms: u64,
    },
    Stopped,
}

#[derive(Default)]
struct RuntimeStats {
    pieces: u64,
    raw_attack: u64,
    cancelled: u64,
    sent: u64,
    received: u64,
    rise_attempted: u64,
    risen: u64,
    gate_considered: u64,
    gate_accepted: u64,
    gate_fallbacks: u64,
}

enum RuntimeAgent {
    Single(AgentController),
    Guard {
        tactical: AgentController,
        cold_clear_fallback: AgentController,
        gate: SafetyGateConfig,
    },
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum GateDecision {
    NotUsed,
    Accepted,
    Fallback,
}

impl RuntimeAgent {
    fn choose(&mut self, observation: &Observation) -> (Option<SelectedAction>, GateDecision) {
        match self {
            Self::Single(agent) => (agent.choose(observation), GateDecision::NotUsed),
            Self::Guard {
                tactical,
                cold_clear_fallback,
                gate,
            } => {
                let tactical_action = tactical.choose(observation);
                let fallback_action = cold_clear_fallback.choose(observation);
                match (tactical_action, fallback_action) {
                    (None, fallback) => (fallback, GateDecision::Fallback),
                    (Some(tactical), None) => (Some(tactical), GateDecision::Accepted),
                    (Some(tactical), Some(fallback)) => {
                        if safety_gate_accepts(observation, &tactical, &fallback, gate) {
                            (Some(tactical), GateDecision::Accepted)
                        } else {
                            (Some(fallback), GateDecision::Fallback)
                        }
                    }
                }
            }
        }
    }
}

#[derive(Copy, Clone, Debug)]
struct ActionSafety {
    locked_out: bool,
    headroom_after_rise: i32,
    holes: u32,
    covered: u32,
    bumpiness: u32,
    accessible_holes: u32,
    cancelled: u32,
    rise: u32,
}

fn safety_gate_accepts(
    observation: &Observation,
    tactical: &SelectedAction,
    fallback: &SelectedAction,
    gate: &SafetyGateConfig,
) -> bool {
    if tactical.action.key() == fallback.action.key() && tactical.wait_ms == fallback.wait_ms {
        return true;
    }
    let tactical_safety = project_action_safety(observation, tactical);
    let fallback_safety = project_action_safety(observation, fallback);
    safety_metrics_accept(&tactical_safety, &fallback_safety, gate)
}

fn safety_metrics_accept(
    tactical_safety: &ActionSafety,
    fallback_safety: &ActionSafety,
    gate: &SafetyGateConfig,
) -> bool {
    if tactical_safety.locked_out {
        return false;
    }
    if fallback_safety.locked_out {
        return true;
    }
    let geometry_safe = tactical_safety.headroom_after_rise
        >= fallback_safety.headroom_after_rise - gate.max_headroom_loss
        && tactical_safety.holes <= fallback_safety.holes + gate.max_hole_increase
        && tactical_safety.covered <= fallback_safety.covered + gate.max_covered_increase
        && tactical_safety.bumpiness <= fallback_safety.bumpiness + gate.max_bumpiness_increase
        && tactical_safety.accessible_holes + gate.max_accessible_hole_loss
            >= fallback_safety.accessible_holes;
    if !geometry_safe {
        return false;
    }
    tactical_safety.headroom_after_rise > fallback_safety.headroom_after_rise
        || tactical_safety.holes < fallback_safety.holes
        || tactical_safety.covered < fallback_safety.covered
        || tactical_safety.bumpiness < fallback_safety.bumpiness
        || tactical_safety.accessible_holes > fallback_safety.accessible_holes
        || tactical_safety.rise < fallback_safety.rise
        || tactical_safety.cancelled
            >= fallback_safety
                .cancelled
                .saturating_add(gate.minimum_cancel_gain)
}

fn project_action_safety(observation: &Observation, selected: &SelectedAction) -> ActionSafety {
    let raw_attack = pc0_attack(&selected.action.lock);
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

struct Runtime {
    board: Board,
    incoming: VecDeque<GarbagePacket>,
    phase: Phase,
    agent: RuntimeAgent,
    piece_rng: StdRng,
    garbage_rng: StdRng,
    last_garbage_hole: Option<usize>,
    dead: bool,
    stats: RuntimeStats,
}

impl Runtime {
    fn view(&self, now_ms: u64) -> PlayerView {
        let phase = match self.phase {
            Phase::Ready | Phase::Stopped => PhaseView::Ready,
            Phase::Moving { started_ms, .. } => PhaseView::Moving { started_ms },
            Phase::LineClear { ends_ms } => PhaseView::LineClear { ends_ms },
        };
        PlayerView {
            board: self.board.clone(),
            can_hold: true,
            incoming: self
                .incoming
                .iter()
                .map(|packet| IncomingPacket {
                    lines: packet.lines,
                    arrival_ms: packet.arrival_ms,
                })
                .collect(),
            phase,
            pieces: self.stats.pieces,
            average_piece_ms: if self.stats.pieces == 0 {
                350.0
            } else {
                now_ms as f32 / self.stats.pieces as f32
            },
        }
    }

    fn replenish_queue(&mut self, count: usize) {
        for _ in 0..count {
            let piece = self.board.generate_next_piece(&mut self.piece_rng);
            self.board.add_next_piece(piece);
        }
    }

    fn cancel_incoming(&mut self, mut attack: u32) -> u32 {
        let original = attack;
        while attack > 0 {
            let Some(front) = self.incoming.front_mut() else {
                break;
            };
            let cancelled = attack.min(front.lines);
            attack -= cancelled;
            front.lines -= cancelled;
            if front.lines == 0 {
                self.incoming.pop_front();
            }
        }
        original - attack
    }

    fn rise_matured(&mut self, now_ms: u64, rules: &Rules) {
        let mut lines = 0;
        let mut retained = VecDeque::with_capacity(self.incoming.len());
        while let Some(packet) = self.incoming.pop_front() {
            if now_ms.saturating_sub(packet.arrival_ms) > rules.garbage_grace_ms {
                lines += packet.lines;
            } else {
                retained.push_back(packet);
            }
        }
        self.incoming = retained;
        self.stats.rise_attempted += lines as u64;
        for _ in 0..lines {
            let mut hole = self
                .last_garbage_hole
                .unwrap_or_else(|| self.garbage_rng.gen_range(0, 10));
            if self.garbage_rng.gen::<f64>() < rules.garbage_randomness {
                hole = self.garbage_rng.gen_range(0, 10);
            }
            self.last_garbage_hole = Some(hole);
            if self.board.add_garbage(hole) {
                self.dead = true;
                self.phase = Phase::Stopped;
                break;
            }
            self.stats.risen += 1;
        }
    }
}

struct MarginTracker {
    area_ms: f64,
    danger_time_ms: u64,
    minimum_headroom: i32,
    maximum_height: u32,
    last_headroom: i32,
    danger_headroom: i32,
}

impl MarginTracker {
    fn new(board: &Board, danger_headroom: i32) -> Self {
        let geometry = BoardGeometry::measure(board);
        let headroom = (20 - geometry.max_height as i32).max(0);
        Self {
            area_ms: 0.0,
            danger_time_ms: 0,
            minimum_headroom: headroom,
            maximum_height: geometry.max_height,
            last_headroom: headroom,
            danger_headroom,
        }
    }

    fn advance(&mut self, delta_ms: u64, alive: bool) {
        if !alive {
            return;
        }
        self.area_ms += self.last_headroom as f64 * delta_ms as f64;
        if self.last_headroom <= self.danger_headroom {
            self.danger_time_ms += delta_ms;
        }
    }

    fn observe(&mut self, board: &Board, dead: bool) {
        let geometry = BoardGeometry::measure(board);
        let headroom = if dead {
            0
        } else {
            (20 - geometry.max_height as i32).max(0)
        };
        self.last_headroom = headroom;
        self.minimum_headroom = self.minimum_headroom.min(headroom);
        self.maximum_height = self.maximum_height.max(geometry.max_height);
    }
}

struct AttackDigest {
    hash: u64,
    packets: u64,
    lines: u64,
}

impl AttackDigest {
    fn new() -> Self {
        Self {
            hash: 0xCBF2_9CE4_8422_2325,
            packets: 0,
            lines: 0,
        }
    }

    fn push(&mut self, at_ms: u64, lines: u32) {
        for value in [at_ms, lines as u64] {
            self.hash ^= value;
            self.hash = self.hash.wrapping_mul(0x0000_0100_0000_01B3);
        }
        self.packets += 1;
        self.lines += lines as u64;
    }
}

struct FixedPressureEngine {
    rules: Rules,
    players: [Runtime; 2],
    now_ms: u64,
    time_limit_ms: u64,
    defender_death_ms: Option<u64>,
    margin: MarginTracker,
    attack: AttackDigest,
}

fn run_fixed_pressure_trial(
    seed: u64,
    defender_cheese_rows: u32,
    defender: Defender,
    base_model: &BaseModel,
    config: &GuardTrainingConfig,
) -> Result<DefenseTrial> {
    let rules = Rules::pinned();
    let attacker_config = AgentConfig {
        cold_clear_nodes: config.attacker_nodes,
        ..AgentConfig::default()
    };
    let defender_agent = match defender {
        Defender::ColdClear => RuntimeAgent::Single(AgentController::new(
            AgentKind::ColdClear,
            AgentConfig {
                cold_clear_nodes: config.defender_nodes,
                ..AgentConfig::default()
            },
        )?),
        Defender::Guard(policy) => RuntimeAgent::Guard {
            tactical: AgentController::new(
                AgentKind::Kasane,
                AgentConfig {
                    cold_clear_nodes: config.defender_nodes,
                    forecast_nodes: config.forecast_nodes,
                    maximum_wait_ms: 0,
                    enable_tank: false,
                    enable_charge: false,
                    strict_base_policy: false,
                    base_depth: config.base_depth,
                    base_beam_width: config.base_beam_width,
                    enable_tempo: true,
                    base_guard_margin: policy.base_guard_margin,
                    base_model_override: Some(base_model.clone()),
                    tempo_model_override: Some(policy.model),
                    ..AgentConfig::default()
                },
            )?,
            cold_clear_fallback: AgentController::new(
                AgentKind::ColdClear,
                AgentConfig {
                    cold_clear_nodes: config.defender_nodes,
                    ..AgentConfig::default()
                },
            )?,
            gate: policy.safety_gate,
        },
    };
    let attacker = make_runtime(
        RuntimeAgent::Single(AgentController::new(AgentKind::ColdClear, attacker_config)?),
        split_seed(seed, 0),
        rules.preview_count,
        0,
    )?;
    let defense = make_runtime(
        defender_agent,
        split_seed(seed, 1),
        rules.preview_count,
        defender_cheese_rows,
    )?;
    let margin = MarginTracker::new(&defense.board, config.danger_headroom);
    FixedPressureEngine {
        rules,
        players: [attacker, defense],
        now_ms: 0,
        time_limit_ms: config.time_limit_ms,
        defender_death_ms: None,
        margin,
        attack: AttackDigest::new(),
    }
    .run(seed, defender_cheese_rows)
}

impl FixedPressureEngine {
    fn run(mut self, seed: u64, defender_cheese_rows: u32) -> Result<DefenseTrial> {
        self.schedule_ready();
        while self.now_ms < self.time_limit_ms {
            let Some(next_ms) = self.next_event_ms() else {
                self.advance_to(self.time_limit_ms);
                break;
            };
            if next_ms > self.time_limit_ms {
                self.advance_to(self.time_limit_ms);
                break;
            }
            self.advance_to(next_ms);
            self.finish_line_delays();
            self.process_locks();
            self.note_defender_state();
            self.schedule_ready();
        }
        let survival_ms = self.defender_death_ms.unwrap_or(self.time_limit_ms);
        let defense = &self.players[1];
        Ok(DefenseTrial {
            seed,
            defender_cheese_rows,
            defense: DefenseMetrics {
                topout: self.defender_death_ms.is_some(),
                survival_ms,
                pieces: defense.stats.pieces,
                raw_attack: defense.stats.raw_attack,
                cancelled: defense.stats.cancelled,
                sent: defense.stats.sent,
                received: defense.stats.received,
                rise_attempted: defense.stats.rise_attempted,
                risen: defense.stats.risen,
                pending_at_end: defense
                    .incoming
                    .iter()
                    .map(|packet| packet.lines as u64)
                    .sum(),
                minimum_headroom: self.margin.minimum_headroom,
                mean_headroom: if survival_ms == 0 {
                    self.margin.last_headroom as f64
                } else {
                    self.margin.area_ms / survival_ms as f64
                },
                maximum_height: self.margin.maximum_height,
                danger_time_ms: self.margin.danger_time_ms,
                gate_considered: defense.stats.gate_considered,
                gate_accepted: defense.stats.gate_accepted,
                gate_fallbacks: defense.stats.gate_fallbacks,
            },
            attack: AttackScheduleSummary {
                packets: self.attack.packets,
                lines: self.attack.lines,
                digest: format!("{:016x}", self.attack.hash),
                source_exhausted: self.players[0].dead,
            },
        })
    }

    fn advance_to(&mut self, next_ms: u64) {
        let delta = next_ms.saturating_sub(self.now_ms);
        self.margin.advance(delta, !self.players[1].dead);
        self.now_ms = next_ms;
    }

    fn next_event_ms(&self) -> Option<u64> {
        self.players
            .iter()
            .filter_map(|player| match player.phase {
                Phase::Moving { lock_ms, .. } => Some(lock_ms),
                Phase::LineClear { ends_ms } => Some(ends_ms),
                Phase::Ready | Phase::Stopped => None,
            })
            .min()
    }

    fn finish_line_delays(&mut self) {
        for index in 0..2 {
            let ends_now = matches!(self.players[index].phase, Phase::LineClear { ends_ms } if ends_ms == self.now_ms);
            if !ends_now {
                continue;
            }
            if index == 1 {
                self.players[index].rise_matured(self.now_ms, &self.rules);
            }
            if !self.players[index].dead {
                self.players[index].phase = Phase::Ready;
            }
        }
    }

    fn process_locks(&mut self) {
        let mut locking: [Option<SelectedAction>; 2] = [None, None];
        for index in 0..2 {
            let should_lock = matches!(self.players[index].phase, Phase::Moving { lock_ms, .. } if lock_ms == self.now_ms);
            if should_lock {
                let old = std::mem::replace(&mut self.players[index].phase, Phase::Ready);
                if let Phase::Moving { selected, .. } = old {
                    locking[index] = Some(selected);
                }
            }
        }
        let mut attacker_outgoing = 0;
        for index in 0..2 {
            let Some(selected) = locking[index].take() else {
                continue;
            };
            let raw_attack = pc0_attack(&selected.action.lock);
            let line_count = selected.action.lock.cleared_lines.len();
            let player = &mut self.players[index];
            player.board = selected.action.board_after;
            player.replenish_queue(selected.action.pieces_consumed);
            player.stats.pieces += 1;
            player.stats.raw_attack += raw_attack as u64;
            if index == 0 {
                attacker_outgoing = raw_attack;
                player.stats.sent += raw_attack as u64;
            } else {
                let cancelled = player.cancel_incoming(raw_attack);
                player.stats.cancelled += cancelled as u64;
                player.stats.sent += raw_attack.saturating_sub(cancelled) as u64;
            }
            if selected.action.lock.locked_out {
                player.dead = true;
                player.phase = Phase::Stopped;
            } else if line_count > 0 {
                player.phase = Phase::LineClear {
                    ends_ms: self.now_ms + self.rules.line_clear_delay_ms,
                };
            } else if index == 1 {
                player.rise_matured(self.now_ms, &self.rules);
                if !player.dead {
                    player.phase = Phase::Ready;
                }
            } else {
                player.phase = Phase::Ready;
            }
        }

        if attacker_outgoing > 0 {
            self.attack.push(self.now_ms, attacker_outgoing);
            if !self.players[1].dead {
                self.players[1].incoming.push_back(GarbagePacket {
                    lines: attacker_outgoing,
                    arrival_ms: self.now_ms,
                });
                self.players[1].stats.received += attacker_outgoing as u64;
            }
        }
    }

    fn note_defender_state(&mut self) {
        self.margin
            .observe(&self.players[1].board, self.players[1].dead);
        if self.players[1].dead && self.defender_death_ms.is_none() {
            self.defender_death_ms = Some(self.now_ms);
        }
    }

    fn schedule_ready(&mut self) {
        let ready = [0, 1].map(|index| {
            !self.players[index].dead && matches!(self.players[index].phase, Phase::Ready)
        });
        if !ready[0] && !ready[1] {
            return;
        }
        let views = [
            self.players[0].view(self.now_ms),
            self.players[1].view(self.now_ms),
        ];
        for index in 0..2 {
            if !ready[index] {
                continue;
            }
            // The pressure source receives a fixed sink view, so defender
            // outgoing attack and board state cannot alter its schedule.
            let opponent = if index == 0 {
                fixed_sink_view()
            } else {
                views[0].clone()
            };
            let observation = Observation {
                now_ms: self.now_ms,
                rules: self.rules.clone(),
                own: views[index].clone(),
                opponent,
            };
            let (selected, gate_decision) = self.players[index].agent.choose(&observation);
            if index == 1 {
                match gate_decision {
                    GateDecision::NotUsed => {}
                    GateDecision::Accepted => {
                        self.players[index].stats.gate_considered += 1;
                        self.players[index].stats.gate_accepted += 1;
                    }
                    GateDecision::Fallback => {
                        self.players[index].stats.gate_considered += 1;
                        self.players[index].stats.gate_fallbacks += 1;
                    }
                }
            }
            let Some(selected) = selected else {
                self.players[index].dead = true;
                self.players[index].phase = Phase::Stopped;
                if index == 1 {
                    self.note_defender_state();
                }
                continue;
            };
            let lock_ms = self.now_ms
                + self.rules.decision_latency_ms
                + selected.wait_ms
                + self
                    .rules
                    .controller_time_ms(selected.action.movements.len(), selected.action.hold);
            self.players[index].phase = Phase::Moving {
                started_ms: self.now_ms,
                lock_ms,
                selected,
            };
        }
    }
}

fn make_runtime(
    agent: RuntimeAgent,
    seed: u64,
    preview_count: usize,
    cheese_rows: u32,
) -> Result<Runtime> {
    let mut field_rng = StdRng::seed_from_u64(split_seed(seed, 10));
    let mut piece_rng = StdRng::seed_from_u64(split_seed(seed, 20));
    let garbage_rng = StdRng::seed_from_u64(split_seed(seed, 30));
    let mut board = Board::new();
    if cheese_rows > 0 {
        board.set_field(cheese_field(cheese_rows, &mut field_rng));
    }
    for _ in 0..=preview_count {
        let piece = board.generate_next_piece(&mut piece_rng);
        board.add_next_piece(piece);
    }
    Ok(Runtime {
        board,
        incoming: VecDeque::new(),
        phase: Phase::Ready,
        agent,
        piece_rng,
        garbage_rng,
        last_garbage_hole: None,
        dead: false,
        stats: RuntimeStats::default(),
    })
}

fn cheese_field(rows: u32, rng: &mut StdRng) -> [[bool; 10]; 40] {
    let mut field = [[false; 10]; 40];
    let mut previous_hole = None;
    for y in 0..rows as usize {
        let mut hole = rng.gen_range(0, 10);
        while previous_hole == Some(hole) {
            hole = rng.gen_range(0, 10);
        }
        previous_hole = Some(hole);
        for x in 0..10 {
            field[y][x] = x != hole;
        }
    }
    field
}

fn fixed_sink_view() -> PlayerView {
    PlayerView {
        board: Board::new(),
        can_hold: true,
        incoming: Vec::new(),
        phase: PhaseView::Ready,
        pieces: 0,
        average_piece_ms: 350.0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn guard_genome_produces_a_valid_pc0_model() {
        let source = TempoModel::default();
        let genes: Vec<_> = GENE_SPECS.iter().map(|spec| spec.initial).collect();
        let policy = policy_from_genome(&source, &genes, &SafetyGateConfig::default()).unwrap();
        policy.model.validate().unwrap();
        let pc = FEATURE_NAMES
            .iter()
            .position(|name| *name == "perfect_clear")
            .unwrap();
        let cancel = FEATURE_NAMES
            .iter()
            .position(|name| *name == "cancelled_own")
            .unwrap();
        assert_eq!(policy.model.linear_weights[pc], 0.0);
        assert!(policy.model.linear_weights[cancel] > 0.0);
    }

    #[test]
    fn paired_trials_receive_the_same_attack_schedule() {
        let mut config = GuardTrainingConfig::default();
        config.attacker_nodes = 1;
        config.defender_nodes = 1;
        config.forecast_nodes = 1;
        config.base_depth = 1;
        config.base_beam_width = 4;
        config.time_limit_ms = 1_500;
        let base = BaseModel::default();
        let genes: Vec<_> = GENE_SPECS.iter().map(|spec| spec.initial).collect();
        let policy =
            policy_from_genome(&TempoModel::default(), &genes, &SafetyGateConfig::default())
                .unwrap();
        let cold = run_fixed_pressure_trial(7, 6, Defender::ColdClear, &base, &config).unwrap();
        let guard =
            run_fixed_pressure_trial(7, 6, Defender::Guard(policy), &base, &config).unwrap();
        assert_eq!(cold.attack.digest, guard.attack.digest);
        assert_eq!(cold.attack.lines, guard.attack.lines);
    }

    #[test]
    fn three_seed_splits_are_disjoint() {
        let config = GuardTrainingConfig::default();
        config.validate().unwrap();
    }

    #[test]
    fn safety_gate_never_trades_headroom_for_cancellation() {
        let gate = SafetyGateConfig::default();
        let fallback = ActionSafety {
            locked_out: false,
            headroom_after_rise: 4,
            holes: 2,
            covered: 3,
            bumpiness: 4,
            accessible_holes: 2,
            cancelled: 1,
            rise: 2,
        };
        let unsafe_tactical = ActionSafety {
            headroom_after_rise: 3,
            cancelled: 4,
            ..fallback
        };
        assert!(!safety_metrics_accept(&unsafe_tactical, &fallback, &gate));

        let safe_counter = ActionSafety {
            cancelled: 2,
            ..fallback
        };
        assert!(safety_metrics_accept(&safe_counter, &fallback, &gate));

        let pointless_deviation = fallback;
        assert!(!safety_metrics_accept(
            &pointless_deviation,
            &fallback,
            &gate
        ));
    }
}
