use anyhow::{bail, ensure, Context, Result};
use clap::Parser;
use kasane::agent::StackRenModel;
use kasane::base::BaseModel;
use kasane::game::{InitialField, PlayerSpec, PlayerStats};
use kasane::model::TempoModel;
use kasane::strategy_training::StrategyPolicyArtifact;
use kasane::{AgentConfig, AgentKind, Match, MatchConfig, MatchOutcome, Rules};
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};
use rayon::prelude::*;
use rayon::{ThreadPool, ThreadPoolBuilder};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

const OUTPUT_WEIGHTS: usize = 32;
const GENOME_SIZE: usize = OUTPUT_WEIGHTS + 1;
const INITIAL_STD: f64 = 0.45;
const STD_FLOOR: f64 = 0.035;
const STD_CEILING: f64 = 2.0;
const MEAN_SMOOTHING: f64 = 0.78;
const STD_SMOOTHING: f64 = 0.68;
const GENE_ABS_LIMIT: f64 = 6.0;

const WIN_REWARD: f64 = 1_000_000.0;
const LOSS_PENALTY: f64 = 1_500_000.0;
const AUXILIARY_LIMIT: f64 = 60_000.0;

#[derive(Debug, Parser)]
#[command(
    name = "train_stack_ren",
    version,
    about = "Train only the 32+1 output head of KASANE Stack-REN v3 with mirrored-seed CEM"
)]
struct Cli {
    #[arg(long, default_value_t = 10)]
    generations: usize,
    #[arg(long, default_value_t = 16)]
    population: usize,
    #[arg(long, default_value_t = 4)]
    elite: usize,
    /// Number of mirrored pairs per candidate (two matches per pair).
    #[arg(long, default_value_t = 8)]
    pairs: usize,
    /// Unseen mirrored pairs used only to select from the training hall of fame.
    #[arg(long, default_value_t = 16)]
    validation_pairs: usize,
    #[arg(long, default_value_t = 8)]
    validation_candidates: usize,
    #[arg(long, default_value_t = 0x4B41_5341_4E45_5303)]
    seed: u64,
    #[arg(long, default_value_t = 30_000)]
    time_limit_ms: u64,
    /// Rayon worker count. Zero uses the global pool/default logical CPU count.
    #[arg(long, default_value_t = 0)]
    threads: usize,
    /// Equal search budget for the Stack-REN Cold Clear floor and its opponent.
    #[arg(long, default_value_t = 300)]
    nodes: u32,
    #[arg(long, default_value = "config/kasane-stack-ren-v3.json")]
    strategy_policy: PathBuf,
    /// Warm-start model. Defaults to `stack_ren_model` in the v3 policy.
    #[arg(long)]
    source_model: Option<PathBuf>,
    #[arg(long, default_value = "models/stack-ren-model-v3.json")]
    output: PathBuf,
    #[arg(long, default_value = "results/stack-ren-training-v3.json")]
    provenance: PathBuf,
}

#[derive(Clone, Debug, Deserialize)]
struct StackRenPolicyArtifact {
    stack_ren_model: PathBuf,
    stack_ren_enable: bool,
    stack_ren_min_well_width: usize,
    stack_ren_max_well_width: usize,
    stack_ren_entry_interval: u32,
    stack_ren_action_limit: usize,
    stack_ren_min_build_pieces: u32,
    stack_ren_max_build_pieces: u32,
    stack_ren_max_build_ms: u64,
    stack_ren_target_depth: u32,
    stack_ren_min_fire_chain: u32,
    stack_ren_min_fire_attack: u32,
    stack_ren_min_headroom: i32,
    stack_ren_max_holes: u32,
    stack_ren_max_due_1000: u32,
    stack_ren_max_forecast_1000: u32,
    stack_ren_entry_threshold: f32,
    stack_ren_fire_margin: f32,
    stack_ren_abort_margin: f32,
    stack_ren_hole_mismatch_penalty: f32,
    stack_ren_cooldown_pieces: u32,
}

impl StackRenPolicyArtifact {
    fn apply_to(&self, config: &mut AgentConfig) {
        config.stack_ren_enable = self.stack_ren_enable;
        config.stack_ren_min_well_width = self.stack_ren_min_well_width;
        config.stack_ren_max_well_width = self.stack_ren_max_well_width;
        config.stack_ren_entry_interval = self.stack_ren_entry_interval;
        config.stack_ren_action_limit = self.stack_ren_action_limit;
        config.stack_ren_min_build_pieces = self.stack_ren_min_build_pieces;
        config.stack_ren_max_build_pieces = self.stack_ren_max_build_pieces;
        config.stack_ren_max_build_ms = self.stack_ren_max_build_ms;
        config.stack_ren_target_depth = self.stack_ren_target_depth;
        config.stack_ren_min_fire_chain = self.stack_ren_min_fire_chain;
        config.stack_ren_min_fire_attack = self.stack_ren_min_fire_attack;
        config.stack_ren_min_headroom = self.stack_ren_min_headroom;
        config.stack_ren_max_holes = self.stack_ren_max_holes;
        config.stack_ren_max_due_1000 = self.stack_ren_max_due_1000;
        config.stack_ren_max_forecast_1000 = self.stack_ren_max_forecast_1000;
        config.stack_ren_entry_threshold = self.stack_ren_entry_threshold;
        config.stack_ren_fire_margin = self.stack_ren_fire_margin;
        config.stack_ren_abort_margin = self.stack_ren_abort_margin;
        config.stack_ren_hole_mismatch_penalty = self.stack_ren_hole_mismatch_penalty;
        config.stack_ren_cooldown_pieces = self.stack_ren_cooldown_pieces;
    }
}

#[derive(Clone)]
struct TrainingContext {
    rules: Rules,
    kasane_config: AgentConfig,
    cold_clear_config: AgentConfig,
    pairs: usize,
    seed: u64,
    time_limit_ms: u64,
}

#[derive(Copy, Clone, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
enum TrainingScenario {
    Empty,
    KasaneCheese6,
    KasaneCheese10,
    ColdClearCheese8,
    BothCheese6,
}

const CURRICULUM: [TrainingScenario; 8] = [
    TrainingScenario::Empty,
    TrainingScenario::Empty,
    TrainingScenario::Empty,
    TrainingScenario::Empty,
    TrainingScenario::KasaneCheese6,
    TrainingScenario::KasaneCheese10,
    TrainingScenario::ColdClearCheese8,
    TrainingScenario::BothCheese6,
];

#[derive(Clone, Debug, Default)]
struct GameMetrics {
    won: bool,
    lost: bool,
    draw: bool,
    timeout: bool,
    ended_ms: u64,
    kasane_sent: u64,
    cold_clear_sent: u64,
    kasane_pieces: u64,
    cold_clear_pieces: u64,
    stack_ren_fires: u64,
    stack_ren_fire_sent: u64,
    stack_ren_continuations: u64,
    max_combo: u32,
}

#[derive(Clone, Debug, Serialize)]
struct Evaluation {
    fitness: f64,
    outcome_component: f64,
    auxiliary_component: f64,
    wins: u64,
    losses: u64,
    draws: u64,
    timeouts: u64,
    matches: u64,
    mean_ended_ms: f64,
    kasane_sent: u64,
    cold_clear_sent: u64,
    sent_difference: i64,
    kasane_pieces: u64,
    cold_clear_pieces: u64,
    kasane_sent_per_piece: f64,
    cold_clear_sent_per_piece: f64,
    sent_per_piece_difference: f64,
    stack_ren_fires: u64,
    stack_ren_fire_sent: u64,
    stack_ren_continuations: u64,
    mean_max_combo: f64,
}

#[derive(Clone, Debug, Serialize)]
struct CandidateRecord {
    generation: usize,
    index: usize,
    genome: Vec<f64>,
    evaluation: Evaluation,
}

#[derive(Clone, Debug, Serialize)]
struct ValidatedCandidateRecord {
    training: CandidateRecord,
    validation: Evaluation,
}

#[derive(Clone, Debug, Serialize)]
struct GenerationRecord {
    generation: usize,
    mean_before: Vec<f64>,
    std_before: Vec<f64>,
    candidates: Vec<CandidateRecord>,
    elite_indices: Vec<usize>,
    mean_after: Vec<f64>,
    std_after: Vec<f64>,
}

#[derive(Debug, Serialize)]
struct TrainingConfigRecord {
    generations: usize,
    population: usize,
    elite: usize,
    pairs: usize,
    validation_pairs: usize,
    validation_candidates: usize,
    matches_per_candidate: usize,
    seed: u64,
    time_limit_ms: u64,
    threads: usize,
    nodes: u32,
    strict_base_policy: bool,
    curriculum: Vec<TrainingScenario>,
    strategy_policy: PathBuf,
    source_model: PathBuf,
    output: PathBuf,
    provenance: PathBuf,
}

#[derive(Debug, Serialize)]
struct AlgorithmRecord {
    family: &'static str,
    sampling: &'static str,
    rng: &'static str,
    initial_std: f64,
    std_floor: f64,
    std_ceiling: f64,
    mean_smoothing: f64,
    std_smoothing: f64,
    gene_abs_limit: f64,
    win_reward: f64,
    loss_penalty: f64,
    auxiliary_limit: f64,
    evolved_parameters: Vec<String>,
    frozen_parameters: Vec<&'static str>,
    fitness_definition: &'static str,
}

#[derive(Debug, Serialize)]
struct ResolvedPolicyRecord {
    schema: String,
    policy_path: PathBuf,
    base_model: PathBuf,
    strategy_model: PathBuf,
    attack_expert_model: PathBuf,
    stack_ren_model: PathBuf,
}

#[derive(Debug, Serialize)]
struct Provenance {
    schema: &'static str,
    contract: &'static str,
    rules: Rules,
    config: TrainingConfigRecord,
    policy: ResolvedPolicyRecord,
    algorithm: AlgorithmRecord,
    generations: Vec<GenerationRecord>,
    selected: CandidateRecord,
    selected_validation: Evaluation,
    validation_hall_of_fame: Vec<ValidatedCandidateRecord>,
    final_distribution_mean: Vec<f64>,
    final_distribution_std: Vec<f64>,
    output_model_schema: String,
}

fn main() -> Result<()> {
    run(Cli::parse())
}

fn run(cli: Cli) -> Result<()> {
    validate_cli(&cli)?;

    let rules = Rules::live();
    rules.validate()?;
    ensure_live_contract(&rules)?;

    let (policy, stack_policy, policy_record) = load_policy(&cli.strategy_policy)?;
    ensure!(
        policy.pc_special_attack == 10,
        "Stack-REN training policy must declare PC10; PC0 is reserved for the bottom-12 benchmark"
    );
    let base_model = BaseModel::load(&policy.base_model).with_context(|| {
        format!(
            "failed to load strategy base model {}",
            policy.base_model.display()
        )
    })?;
    let strategy_model = TempoModel::load(&policy.strategy_model).with_context(|| {
        format!(
            "failed to load strategy model {}",
            policy.strategy_model.display()
        )
    })?;

    let mut kasane_config = AgentConfig::default();
    policy.apply_to(&mut kasane_config);
    stack_policy.apply_to(&mut kasane_config);
    // Match the shipped WASM policy exactly. Strategy v2 and Stack-REN v3
    // are allowed to rerank ordinary zero-wait moves in the browser.
    kasane_config.strict_base_policy = false;
    kasane_config.cold_clear_nodes = cli.nodes;
    kasane_config.kasane_nodes = cli.nodes;
    kasane_config.stack_ren_model_path = None;
    kasane_config.base_model_override = Some(base_model);
    kasane_config.tempo_model_override = Some(strategy_model);

    let cold_clear_config = AgentConfig {
        cold_clear_nodes: cli.nodes,
        kasane_nodes: cli.nodes,
        ..AgentConfig::default()
    };
    let context = TrainingContext {
        rules: rules.clone(),
        kasane_config,
        cold_clear_config,
        pairs: cli.pairs,
        seed: cli.seed,
        time_limit_ms: cli.time_limit_ms,
    };

    let source_model = cli
        .source_model
        .clone()
        .unwrap_or_else(|| stack_policy.stack_ren_model.clone());
    let template = StackRenModel::load(&source_model).with_context(|| {
        format!(
            "failed to load Stack-REN source model {}",
            source_model.display()
        )
    })?;
    template.validate()?;
    ensure!(
        template.neural_output_weights.len() == OUTPUT_WEIGHTS,
        "StackRenModel output width changed: expected {OUTPUT_WEIGHTS}, got {}",
        template.neural_output_weights.len()
    );

    let pool = if cli.threads == 0 {
        None
    } else {
        Some(
            ThreadPoolBuilder::new()
                .num_threads(cli.threads)
                .build()
                .context("failed to build the requested Rayon thread pool")?,
        )
    };

    let mut sampling_rng = StdRng::seed_from_u64(mix_seed(cli.seed, 0x4345_4D53));
    let mut mean = model_genome(&template)?;
    let mut std = vec![INITIAL_STD; GENOME_SIZE];
    let mut generations = Vec::with_capacity(cli.generations);

    for generation in 0..cli.generations {
        let mean_before = mean.clone();
        let std_before = std.clone();
        let genomes =
            sample_population(&mean_before, &std_before, cli.population, &mut sampling_rng);
        let models = genomes
            .iter()
            .map(|genome| model_from_genome(&template, genome))
            .collect::<Result<Vec<_>>>()?;
        let evaluations = evaluate_population(&models, &context, pool.as_ref())?;
        let mut candidates = genomes
            .into_iter()
            .zip(evaluations)
            .enumerate()
            .map(|(index, (genome, evaluation))| CandidateRecord {
                generation,
                index,
                genome,
                evaluation,
            })
            .collect::<Vec<_>>();
        candidates.sort_by(compare_candidates);

        update_distribution(&mut mean, &mut std, &candidates[..cli.elite]);
        let elite_indices = candidates[..cli.elite]
            .iter()
            .map(|candidate| candidate.index)
            .collect::<Vec<_>>();
        let best = &candidates[0];
        println!(
            "generation={}/{} fitness={:.3} W-L-D-T={}-{}-{}-{} sent_diff={} stack_fires={} stack_fire_sent={} sent_per_piece_delta={:.5} elite={:?}",
            generation + 1,
            cli.generations,
            best.evaluation.fitness,
            best.evaluation.wins,
            best.evaluation.losses,
            best.evaluation.draws,
            best.evaluation.timeouts,
            best.evaluation.sent_difference,
            best.evaluation.stack_ren_fires,
            best.evaluation.stack_ren_fire_sent,
            best.evaluation.sent_per_piece_difference,
            elite_indices,
        );
        generations.push(GenerationRecord {
            generation,
            mean_before,
            std_before,
            candidates,
            elite_indices,
            mean_after: mean.clone(),
            std_after: std.clone(),
        });
    }

    let source_genome = model_genome(&template)?;
    let source_candidate = generations
        .first()
        .and_then(|generation| {
            generation
                .candidates
                .iter()
                .find(|candidate| candidate.genome == source_genome)
        })
        .cloned()
        .context("generation 1 did not retain the exact source-model candidate")?;
    let mut ranked = generations
        .iter()
        .flat_map(|generation| generation.candidates.iter().cloned())
        .collect::<Vec<_>>();
    ranked.sort_by(compare_candidates);
    let mut hall = Vec::with_capacity(cli.validation_candidates);
    for candidate in ranked {
        if hall
            .iter()
            .any(|current: &CandidateRecord| current.genome == candidate.genome)
        {
            continue;
        }
        hall.push(candidate);
        if hall.len() >= cli.validation_candidates {
            break;
        }
    }
    if !hall
        .iter()
        .any(|candidate| candidate.genome == source_genome)
    {
        if hall.len() >= cli.validation_candidates {
            hall.pop();
        }
        hall.push(source_candidate);
    }
    ensure!(!hall.is_empty(), "CEM produced no candidates");
    let validation_models = hall
        .iter()
        .map(|candidate| model_from_genome(&template, &candidate.genome))
        .collect::<Result<Vec<_>>>()?;
    let validation_context = TrainingContext {
        pairs: cli.validation_pairs,
        seed: mix_seed(cli.seed, 0x5641_4C49_4441_5445),
        ..context.clone()
    };
    let validation_evaluations =
        evaluate_population(&validation_models, &validation_context, pool.as_ref())?;
    let mut validation_hall_of_fame = hall
        .into_iter()
        .zip(validation_evaluations)
        .map(|(training, validation)| ValidatedCandidateRecord {
            training,
            validation,
        })
        .collect::<Vec<_>>();
    validation_hall_of_fame.sort_by(compare_validated_candidates);
    let winner = validation_hall_of_fame
        .first()
        .context("validation produced no candidates")?;
    let selected = winner.training.clone();
    let selected_validation = winner.validation.clone();
    let selected_model = model_from_genome(&template, &selected.genome)?;
    ensure_frozen_model_unchanged(&template, &selected_model)?;
    create_parent(&cli.output)?;
    selected_model
        .save(&cli.output)
        .with_context(|| format!("failed to save model {}", cli.output.display()))?;

    let provenance = Provenance {
        schema: "kasane-stack-ren-training/v3-cem-mirrored",
        contract: "PC10 live duel; input=50ms, line-clear=750ms, garbage-grace=1000ms; browser-equivalent free rerank; equal nodes; mirrored seats and seed; 50% empty plus deterministic strict-cheese accident curriculum; only neural_output_weights[0..32] and neural_output_bias evolve",
        rules,
        config: TrainingConfigRecord {
            generations: cli.generations,
            population: cli.population,
            elite: cli.elite,
            pairs: cli.pairs,
            validation_pairs: cli.validation_pairs,
            validation_candidates: cli.validation_candidates,
            matches_per_candidate: cli.pairs * 2,
            seed: cli.seed,
            time_limit_ms: cli.time_limit_ms,
            threads: cli.threads,
            nodes: cli.nodes,
            strict_base_policy: false,
            curriculum: CURRICULUM.to_vec(),
            strategy_policy: cli.strategy_policy.clone(),
            source_model,
            output: cli.output.clone(),
            provenance: cli.provenance.clone(),
        },
        policy: policy_record,
        algorithm: algorithm_record(),
        generations,
        selected: selected.clone(),
        selected_validation: selected_validation.clone(),
        validation_hall_of_fame,
        final_distribution_mean: mean,
        final_distribution_std: std,
        output_model_schema: selected_model.schema.clone(),
    };
    create_parent(&cli.provenance)?;
    fs::write(&cli.provenance, serde_json::to_vec_pretty(&provenance)?).with_context(|| {
        format!(
            "failed to save training provenance {}",
            cli.provenance.display()
        )
    })?;

    println!(
        "selected_generation={} selected_candidate={} train_fitness={:.3} validation_fitness={:.3} validation_W-L-D-T={}-{}-{}-{} model={} provenance={}",
        selected.generation + 1,
        selected.index,
        selected.evaluation.fitness,
        selected_validation.fitness,
        selected_validation.wins,
        selected_validation.losses,
        selected_validation.draws,
        selected_validation.timeouts,
        cli.output.display(),
        cli.provenance.display(),
    );
    Ok(())
}

fn validate_cli(cli: &Cli) -> Result<()> {
    if cli.generations == 0 {
        bail!("generations must be positive");
    }
    if cli.population < 2 {
        bail!("population must be at least 2");
    }
    if cli.elite == 0 || cli.elite > cli.population {
        bail!("elite must be in 1..=population");
    }
    if cli.pairs == 0 {
        bail!("pairs must be positive");
    }
    if cli.validation_pairs == 0 || cli.validation_candidates == 0 {
        bail!("validation-pairs and validation-candidates must be positive");
    }
    if cli.time_limit_ms == 0 {
        bail!("time-limit-ms must be positive");
    }
    if cli.nodes == 0 {
        bail!("nodes must be positive");
    }
    if cli.output == cli.provenance {
        bail!("output and provenance must be different files");
    }
    cli.pairs
        .checked_mul(2)
        .and_then(|matches| matches.checked_mul(cli.population))
        .context("population * pairs * 2 overflows usize")?;
    Ok(())
}

fn ensure_live_contract(rules: &Rules) -> Result<()> {
    ensure!(rules.input_interval_ms == 50, "input interval drifted");
    ensure!(rules.line_clear_delay_ms == 750, "line-clear delay drifted");
    ensure!(rules.garbage_grace_ms == 1_000, "garbage grace drifted");
    ensure!(
        rules.perfect_clear_special_attack == 10,
        "training must use the live PC10 attack"
    );
    Ok(())
}

fn load_policy(
    path: &Path,
) -> Result<(
    StrategyPolicyArtifact,
    StackRenPolicyArtifact,
    ResolvedPolicyRecord,
)> {
    let text = fs::read_to_string(path)
        .with_context(|| format!("failed to read strategy policy {}", path.display()))?;
    let mut policy: StrategyPolicyArtifact = serde_json::from_str(&text)
        .with_context(|| format!("failed to parse strategy policy {}", path.display()))?;
    let mut stack_policy: StackRenPolicyArtifact = serde_json::from_str(&text)
        .with_context(|| format!("failed to parse Stack-REN policy {}", path.display()))?;
    policy.base_model = resolve_embedded_path(path, &policy.base_model);
    policy.strategy_model = resolve_embedded_path(path, &policy.strategy_model);
    policy.attack_expert_model = resolve_embedded_path(path, &policy.attack_expert_model);
    stack_policy.stack_ren_model = resolve_embedded_path(path, &stack_policy.stack_ren_model);
    let record = ResolvedPolicyRecord {
        schema: policy.schema.clone(),
        policy_path: path.to_path_buf(),
        base_model: policy.base_model.clone(),
        strategy_model: policy.strategy_model.clone(),
        attack_expert_model: policy.attack_expert_model.clone(),
        stack_ren_model: stack_policy.stack_ren_model.clone(),
    };
    Ok((policy, stack_policy, record))
}

fn resolve_embedded_path(policy_path: &Path, embedded: &Path) -> PathBuf {
    if embedded.is_absolute() || embedded.exists() {
        return embedded.to_path_buf();
    }
    let Some(policy_dir) = policy_path.parent() else {
        return embedded.to_path_buf();
    };
    let project_candidate = policy_dir
        .parent()
        .map(|project_dir| project_dir.join(embedded));
    if let Some(candidate) = project_candidate {
        if candidate.exists() {
            return candidate;
        }
    }
    let config_relative = policy_dir.join(embedded);
    if config_relative.exists() {
        return config_relative;
    }
    embedded.to_path_buf()
}

fn model_genome(model: &StackRenModel) -> Result<Vec<f64>> {
    ensure!(
        model.neural_output_weights.len() == OUTPUT_WEIGHTS,
        "expected {OUTPUT_WEIGHTS} neural output weights"
    );
    let mut genome = model
        .neural_output_weights
        .iter()
        .map(|&value| value as f64)
        .collect::<Vec<_>>();
    genome.push(model.neural_output_bias as f64);
    Ok(genome)
}

fn model_from_genome(template: &StackRenModel, genome: &[f64]) -> Result<StackRenModel> {
    ensure!(
        genome.len() == GENOME_SIZE,
        "expected a {GENOME_SIZE}-value Stack-REN genome"
    );
    ensure!(
        genome.iter().all(|value| value.is_finite()),
        "Stack-REN genome contains a non-finite value"
    );
    let mut model = template.clone();
    model.neural_output_weights = genome[..OUTPUT_WEIGHTS]
        .iter()
        .map(|&value| value as f32)
        .collect();
    model.neural_output_bias = genome[OUTPUT_WEIGHTS] as f32;
    model.validate()?;
    ensure_frozen_model_unchanged(template, &model)?;
    Ok(model)
}

fn ensure_frozen_model_unchanged(
    template: &StackRenModel,
    candidate: &StackRenModel,
) -> Result<()> {
    ensure!(
        candidate.schema == template.schema,
        "model schema was mutated"
    );
    ensure!(
        candidate.feature_names == template.feature_names,
        "feature names were mutated"
    );
    ensure!(
        candidate.input_mean == template.input_mean,
        "input mean was mutated"
    );
    ensure!(
        candidate.input_scale == template.input_scale,
        "input scale was mutated"
    );
    ensure!(
        candidate.linear_weights == template.linear_weights,
        "linear weights were mutated"
    );
    ensure!(
        candidate.linear_bias == template.linear_bias,
        "linear bias was mutated"
    );
    ensure!(
        candidate.hidden_layers.len() == template.hidden_layers.len(),
        "hidden layer count was mutated"
    );
    for (index, (actual, expected)) in candidate
        .hidden_layers
        .iter()
        .zip(&template.hidden_layers)
        .enumerate()
    {
        ensure!(
            actual.weights == expected.weights && actual.bias == expected.bias,
            "hidden layer {index} was mutated"
        );
    }
    Ok(())
}

fn sample_population(
    mean: &[f64],
    std: &[f64],
    population: usize,
    rng: &mut StdRng,
) -> Vec<Vec<f64>> {
    debug_assert_eq!(mean.len(), GENOME_SIZE);
    debug_assert_eq!(std.len(), GENOME_SIZE);
    let mut samples = Vec::with_capacity(population);
    // Retaining the current mean makes every generation no-regret on its
    // sampled match set. Remaining candidates are antithetic pairs, reducing
    // CEM noise without changing the deterministic RNG stream.
    samples.push(mean.to_vec());
    while samples.len() < population {
        let noise = (0..GENOME_SIZE)
            .map(|_| standard_normal(rng))
            .collect::<Vec<_>>();
        let positive = mean
            .iter()
            .zip(std)
            .zip(&noise)
            .map(|((&center, &spread), &z)| clamp_gene(center + spread * z))
            .collect::<Vec<_>>();
        samples.push(positive);
        if samples.len() < population {
            let negative = mean
                .iter()
                .zip(std)
                .zip(&noise)
                .map(|((&center, &spread), &z)| clamp_gene(center - spread * z))
                .collect::<Vec<_>>();
            samples.push(negative);
        }
    }
    samples
}

fn standard_normal(rng: &mut StdRng) -> f64 {
    let u1 = rng.gen::<f64>().max(f64::MIN_POSITIVE);
    let u2 = rng.gen::<f64>();
    (-2.0 * u1.ln()).sqrt() * (std::f64::consts::TAU * u2).cos()
}

fn clamp_gene(value: f64) -> f64 {
    value.clamp(-GENE_ABS_LIMIT, GENE_ABS_LIMIT)
}

fn evaluate_population(
    models: &[StackRenModel],
    context: &TrainingContext,
    pool: Option<&ThreadPool>,
) -> Result<Vec<Evaluation>> {
    let games_per_candidate = context
        .pairs
        .checked_mul(2)
        .context("pairs * 2 overflows usize")?;
    let jobs = models
        .len()
        .checked_mul(games_per_candidate)
        .context("population match count overflows usize")?;
    let execute = || {
        (0..jobs)
            .into_par_iter()
            .map(|job| {
                let candidate = job / games_per_candidate;
                let game = job % games_per_candidate;
                let pair = game / 2;
                let kasane_first = game % 2 == 0;
                let seed = match_seed(context.seed, pair as u64);
                let scenario = CURRICULUM[pair % CURRICULUM.len()];
                let metrics = run_match(&models[candidate], context, seed, kasane_first, scenario)?;
                Ok((job, candidate, metrics))
            })
            .collect::<Result<Vec<_>>>()
    };
    let mut results = if let Some(pool) = pool {
        pool.install(execute)?
    } else {
        execute()?
    };
    // Indexed rayon collection is ordered, but sorting explicitly keeps the
    // reduction deterministic if this iterator is refactored later.
    results.sort_by_key(|(job, _, _)| *job);
    let mut grouped = (0..models.len())
        .map(|_| Vec::with_capacity(games_per_candidate))
        .collect::<Vec<_>>();
    for (_, candidate, metrics) in results {
        grouped[candidate].push(metrics);
    }
    grouped
        .iter()
        .map(|games| summarize(games))
        .collect::<Result<Vec<_>>>()
}

fn run_match(
    model: &StackRenModel,
    context: &TrainingContext,
    seed: u64,
    kasane_first: bool,
    scenario: TrainingScenario,
) -> Result<GameMetrics> {
    let cheese = |rows| InitialField::Cheese {
        rows,
        strict_hole_bara: true,
    };
    let (kasane_field, cold_clear_field) = match scenario {
        TrainingScenario::Empty => (InitialField::Empty, InitialField::Empty),
        TrainingScenario::KasaneCheese6 => (cheese(6), InitialField::Empty),
        TrainingScenario::KasaneCheese10 => (cheese(10), InitialField::Empty),
        TrainingScenario::ColdClearCheese8 => (InitialField::Empty, cheese(8)),
        TrainingScenario::BothCheese6 => (cheese(6), cheese(6)),
    };
    let (players, kasane_index) = if kasane_first {
        (
            [
                PlayerSpec {
                    agent: AgentKind::KasaneStackRen,
                    initial_field: kasane_field,
                },
                PlayerSpec {
                    agent: AgentKind::ColdClear,
                    initial_field: cold_clear_field,
                },
            ],
            0,
        )
    } else {
        (
            [
                PlayerSpec {
                    agent: AgentKind::ColdClear,
                    initial_field: cold_clear_field,
                },
                PlayerSpec {
                    agent: AgentKind::KasaneStackRen,
                    initial_field: kasane_field,
                },
            ],
            1,
        )
    };
    let mut kasane_config = context.kasane_config.clone();
    kasane_config.stack_ren_model_override = Some(model.clone());
    let agent_configs = if kasane_first {
        [kasane_config, context.cold_clear_config.clone()]
    } else {
        [context.cold_clear_config.clone(), kasane_config]
    };
    let result = Match::new(MatchConfig {
        rules: context.rules.clone(),
        players,
        agent_configs,
        seed,
        time_limit_ms: context.time_limit_ms,
        record_trace: false,
    })?
    .run()?;
    let cold_clear_index = 1 - kasane_index;
    let won = matches!(
        (kasane_index, result.outcome),
        (0, MatchOutcome::Player0Win) | (1, MatchOutcome::Player1Win)
    );
    let lost = matches!(
        (kasane_index, result.outcome),
        (0, MatchOutcome::Player1Win) | (1, MatchOutcome::Player0Win)
    );
    let kasane = &result.stats[kasane_index];
    let cold_clear = &result.stats[cold_clear_index];
    Ok(game_metrics(
        won,
        lost,
        result.outcome,
        result.ended_ms,
        kasane,
        cold_clear,
    ))
}

fn game_metrics(
    won: bool,
    lost: bool,
    outcome: MatchOutcome,
    ended_ms: u64,
    kasane: &PlayerStats,
    cold_clear: &PlayerStats,
) -> GameMetrics {
    GameMetrics {
        won,
        lost,
        draw: outcome == MatchOutcome::Draw,
        timeout: outcome == MatchOutcome::Timeout,
        ended_ms,
        kasane_sent: kasane.sent,
        cold_clear_sent: cold_clear.sent,
        kasane_pieces: kasane.pieces,
        cold_clear_pieces: cold_clear.pieces,
        stack_ren_fires: kasane.stack_ren_fires,
        stack_ren_fire_sent: kasane.stack_ren_fire_sent,
        stack_ren_continuations: kasane.stack_ren_continuations,
        max_combo: kasane.max_combo,
    }
}

fn summarize(games: &[GameMetrics]) -> Result<Evaluation> {
    ensure!(!games.is_empty(), "candidate has no mirrored matches");
    let matches = games.len() as u64;
    let wins = games.iter().filter(|game| game.won).count() as u64;
    let losses = games.iter().filter(|game| game.lost).count() as u64;
    let draws = games.iter().filter(|game| game.draw).count() as u64;
    let timeouts = games.iter().filter(|game| game.timeout).count() as u64;
    let ended_ms = games.iter().map(|game| game.ended_ms).sum::<u64>();
    let kasane_sent = games.iter().map(|game| game.kasane_sent).sum::<u64>();
    let cold_clear_sent = games.iter().map(|game| game.cold_clear_sent).sum::<u64>();
    let kasane_pieces = games.iter().map(|game| game.kasane_pieces).sum::<u64>();
    let cold_clear_pieces = games.iter().map(|game| game.cold_clear_pieces).sum::<u64>();
    let stack_ren_fires = games.iter().map(|game| game.stack_ren_fires).sum();
    let stack_ren_fire_sent = games.iter().map(|game| game.stack_ren_fire_sent).sum();
    let stack_ren_continuations = games.iter().map(|game| game.stack_ren_continuations).sum();
    let combo_sum = games.iter().map(|game| game.max_combo as u64).sum::<u64>();
    let sent_difference = signed_difference(kasane_sent, cold_clear_sent);
    let kasane_sent_per_piece = kasane_sent as f64 / kasane_pieces.max(1) as f64;
    let cold_clear_sent_per_piece = cold_clear_sent as f64 / cold_clear_pieces.max(1) as f64;
    let sent_per_piece_difference = kasane_sent_per_piece - cold_clear_sent_per_piece;

    // A single outcome is worth more than the entire bounded auxiliary term.
    // Losses cost 1.5 wins, making unsafe stack-entry policies uncompetitive.
    let outcome_component = wins as f64 * WIN_REWARD - losses as f64 * LOSS_PENALTY;
    let sent_component = (sent_difference as f64 * 30.0).clamp(-25_000.0, 25_000.0);
    let ren_component = (stack_ren_fire_sent as f64 * 45.0
        + stack_ren_fires as f64 * 650.0
        + stack_ren_continuations as f64 * 90.0
        + combo_sum as f64 * 35.0)
        .min(25_000.0);
    let efficiency_component = (sent_per_piece_difference * 5_000.0).clamp(-10_000.0, 10_000.0);
    let auxiliary_component = (sent_component + ren_component + efficiency_component)
        .clamp(-AUXILIARY_LIMIT, AUXILIARY_LIMIT);

    Ok(Evaluation {
        fitness: outcome_component + auxiliary_component,
        outcome_component,
        auxiliary_component,
        wins,
        losses,
        draws,
        timeouts,
        matches,
        mean_ended_ms: ended_ms as f64 / matches as f64,
        kasane_sent,
        cold_clear_sent,
        sent_difference,
        kasane_pieces,
        cold_clear_pieces,
        kasane_sent_per_piece,
        cold_clear_sent_per_piece,
        sent_per_piece_difference,
        stack_ren_fires,
        stack_ren_fire_sent,
        stack_ren_continuations,
        mean_max_combo: combo_sum as f64 / matches as f64,
    })
}

fn signed_difference(left: u64, right: u64) -> i64 {
    let difference = left as i128 - right as i128;
    difference.clamp(i64::MIN as i128, i64::MAX as i128) as i64
}

fn compare_candidates(left: &CandidateRecord, right: &CandidateRecord) -> std::cmp::Ordering {
    right
        .evaluation
        .fitness
        .total_cmp(&left.evaluation.fitness)
        .then_with(|| left.index.cmp(&right.index))
}

fn compare_validated_candidates(
    left: &ValidatedCandidateRecord,
    right: &ValidatedCandidateRecord,
) -> std::cmp::Ordering {
    right
        .validation
        .fitness
        .total_cmp(&left.validation.fitness)
        .then_with(|| compare_candidates(&left.training, &right.training))
}

fn update_distribution(mean: &mut [f64], std: &mut [f64], elites: &[CandidateRecord]) {
    debug_assert!(!elites.is_empty());
    for dimension in 0..GENOME_SIZE {
        let elite_mean = elites
            .iter()
            .map(|candidate| candidate.genome[dimension])
            .sum::<f64>()
            / elites.len() as f64;
        let variance = elites
            .iter()
            .map(|candidate| (candidate.genome[dimension] - elite_mean).powi(2))
            .sum::<f64>()
            / elites.len() as f64;
        let target_std = variance.sqrt().clamp(STD_FLOOR, STD_CEILING);
        mean[dimension] =
            clamp_gene((1.0 - MEAN_SMOOTHING) * mean[dimension] + MEAN_SMOOTHING * elite_mean);
        std[dimension] = ((1.0 - STD_SMOOTHING) * std[dimension] + STD_SMOOTHING * target_std)
            .clamp(STD_FLOOR, STD_CEILING);
    }
}

fn mix_seed(seed: u64, stream: u64) -> u64 {
    let mut value = seed ^ stream.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    value ^= value >> 30;
    value = value.wrapping_mul(0xBF58_476D_1CE4_E5B9);
    value ^= value >> 27;
    value = value.wrapping_mul(0x94D0_49BB_1331_11EB);
    value ^ (value >> 31)
}

fn match_seed(seed: u64, pair: u64) -> u64 {
    mix_seed(seed ^ 0x4D49_5252_4F52_4544, pair)
}

fn algorithm_record() -> AlgorithmRecord {
    let mut evolved_parameters = (0..OUTPUT_WEIGHTS)
        .map(|index| format!("neural_output_weights[{index}]"))
        .collect::<Vec<_>>();
    evolved_parameters.push("neural_output_bias".to_owned());
    AlgorithmRecord {
        family: "cross-entropy method (elite-selection evolutionary search)",
        sampling: "current mean plus deterministic antithetic Gaussian pairs",
        rng: "rand 0.7 StdRng seeded once; fixed common mirrored match seeds",
        initial_std: INITIAL_STD,
        std_floor: STD_FLOOR,
        std_ceiling: STD_CEILING,
        mean_smoothing: MEAN_SMOOTHING,
        std_smoothing: STD_SMOOTHING,
        gene_abs_limit: GENE_ABS_LIMIT,
        win_reward: WIN_REWARD,
        loss_penalty: LOSS_PENALTY,
        auxiliary_limit: AUXILIARY_LIMIT,
        evolved_parameters,
        frozen_parameters: vec![
            "schema",
            "feature_names",
            "input_mean",
            "input_scale",
            "linear_weights",
            "linear_bias",
            "hidden_layers[56x64,64x32] weights and biases",
        ],
        fitness_definition: "wins*1,000,000 - losses*1,500,000 + clamp(sent differential + Stack-REN fire/sent/continuation/combo signal + sent-per-piece differential, +/-60,000)",
    }
}

fn create_parent(path: &Path) -> Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)
                .with_context(|| format!("failed to create {}", parent.display()))?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn genome_mutates_only_the_output_head() {
        let template = StackRenModel::bootstrap();
        let genome = (0..GENOME_SIZE)
            .map(|index| index as f64 / 100.0)
            .collect::<Vec<_>>();
        let model = model_from_genome(&template, &genome).unwrap();
        ensure_frozen_model_unchanged(&template, &model).unwrap();
        assert_eq!(
            model.neural_output_weights,
            genome[..OUTPUT_WEIGHTS]
                .iter()
                .map(|value| *value as f32)
                .collect::<Vec<_>>()
        );
        assert_eq!(model.neural_output_bias, genome[OUTPUT_WEIGHTS] as f32);
    }

    #[test]
    fn sampling_is_seed_deterministic_and_antithetic() {
        let mean = vec![0.0; GENOME_SIZE];
        let std = vec![INITIAL_STD; GENOME_SIZE];
        let mut left_rng = StdRng::seed_from_u64(17);
        let mut right_rng = StdRng::seed_from_u64(17);
        let left = sample_population(&mean, &std, 5, &mut left_rng);
        let right = sample_population(&mean, &std, 5, &mut right_rng);
        assert_eq!(left, right);
        assert_eq!(left[0], mean);
        for dimension in 0..GENOME_SIZE {
            assert!((left[1][dimension] + left[2][dimension]).abs() < 1e-12);
        }
    }

    #[test]
    fn one_loss_dominates_every_auxiliary_reward() {
        let safe = summarize(&[GameMetrics {
            timeout: true,
            ..GameMetrics::default()
        }])
        .unwrap();
        let unsafe_policy = summarize(&[GameMetrics {
            lost: true,
            kasane_sent: 10_000,
            stack_ren_fires: 1_000,
            stack_ren_fire_sent: 10_000,
            stack_ren_continuations: 10_000,
            max_combo: 1_000,
            kasane_pieces: 1,
            cold_clear_pieces: 10_000,
            ..GameMetrics::default()
        }])
        .unwrap();
        assert!(safe.fitness > unsafe_policy.fitness);
        assert!(unsafe_policy.auxiliary_component <= AUXILIARY_LIMIT);
    }
}
