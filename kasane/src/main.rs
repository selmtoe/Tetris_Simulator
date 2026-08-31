use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use kasane::agent::{AgentConfig, AgentKind};
use kasane::base::BaseModel;
use kasane::benchmark::{run_cheese_benchmark, CheeseBenchmarkConfig};
use kasane::evolution::{evolve_base, evolve_tempo, EvolutionConfig, TempoEvolutionConfig};
use kasane::model::TempoModel;
use kasane::strategy_training::StrategyPolicyArtifact;
use kasane::training::{generate_base_dataset, BaseDatasetConfig};
use std::fs;
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "kasane", version, about = "Time-aware strategic Tetris AI")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Compare Cold Clear and KASANE as attackers against 12-row strict cheese.
    Benchmark {
        #[arg(long, default_value_t = 100)]
        games: usize,
        #[arg(long, default_value_t = 30_000)]
        time_limit_ms: u64,
        #[arg(long, default_value_t = 0x4B41_5341_4E45_0001)]
        seed: u64,
        #[arg(long, default_value_t = 1_500)]
        cold_clear_nodes: u32,
        /// Node budget for KASANE's embedded Cold Clear safety floor. The
        /// independent base search uses `base_depth` and `base_beam_width`.
        #[arg(long, default_value_t = 1_500)]
        kasane_nodes: u32,
        #[arg(long)]
        forecast_nodes: Option<u32>,
        #[arg(long, default_value_t = 4)]
        base_depth: usize,
        #[arg(long, default_value_t = 64)]
        base_beam_width: usize,
        #[arg(long, default_value_t = 0)]
        threads: usize,
        #[arg(long)]
        base_model: Option<PathBuf>,
        #[arg(long)]
        tempo_model: Option<PathBuf>,
        /// Apply every MoE/stateful threshold from this policy before explicit
        /// CLI node/model overrides.
        #[arg(long)]
        strategy_policy: Option<PathBuf>,
        /// Evaluate the independent Stack-REN v3 attacker.
        #[arg(long)]
        stack_ren: bool,
        /// Override the Stack-REN neural gate artifact.
        #[arg(long)]
        stack_ren_model: Option<PathBuf>,
        #[arg(long, default_value_t = false)]
        disable_tempo: bool,
        /// Enable the experimental two-ply charge branch.
        #[arg(long, default_value_t = false)]
        enable_charge: bool,
        /// Let Tempo rerank ordinary zero-wait moves outside KASANE-Base.
        #[arg(long, default_value_t = false)]
        allow_free_rerank: bool,
        #[arg(long, default_value_t = 6.0)]
        base_guard_margin: f32,
        #[arg(long, default_value_t = 750)]
        dodge_wait_cap_ms: u64,
        #[arg(long, default_value_t = 13)]
        dodge_finish_height: u32,
        #[arg(long, default_value_t = 5)]
        dodge_safety_margin: i32,
        #[arg(long)]
        strategy_min_advantage: Option<f32>,
        #[arg(long)]
        strategy_placement_min_advantage: Option<f32>,
        #[arg(long)]
        strategy_max_alternatives: Option<usize>,
        #[arg(long)]
        strategy_max_wait_ms: Option<u64>,
        #[arg(long, default_value_t = false)]
        disable_stateful_strategy: bool,
        #[arg(long)]
        output: Option<PathBuf>,
    },
    /// Write the transparent bootstrap TempoNet model as JSON.
    ExportDefaultModel {
        #[arg(long, default_value = "config/tempo-model-bootstrap-v3.json")]
        output: PathBuf,
    },
    /// Write the independent bootstrap stacking model as JSON.
    ExportDefaultBaseModel {
        #[arg(long, default_value = "config/base-model-heuristic-v1.json")]
        output: PathBuf,
    },
    /// Generate diverse Cold Clear teacher rankings for offline warm-start.
    GenerateBaseDataset {
        #[arg(long, default_value_t = 5_000)]
        states: usize,
        #[arg(long, default_value_t = 40)]
        episode_length: usize,
        #[arg(long, default_value_t = 500)]
        teacher_nodes: u32,
        #[arg(long, default_value_t = 0x4B41_5341_4E45_D157)]
        seed: u64,
        #[arg(long, default_value_t = 0)]
        threads: usize,
        #[arg(long, default_value = "data/base-distillation-v1.jsonl")]
        output: PathBuf,
        #[arg(long)]
        behavior_model: Option<PathBuf>,
        #[arg(long, default_value_t = 0.70)]
        behavior_probability: f64,
    },
    /// Optimize the compact KASANE evaluator genome on exact 20-second matches.
    EvolveBase {
        #[arg(long, default_value_t = 16)]
        population: usize,
        #[arg(long, default_value_t = 5)]
        elites: usize,
        #[arg(long, default_value_t = 8)]
        generations: usize,
        #[arg(long, default_value_t = 24)]
        games_per_candidate: usize,
        #[arg(long, default_value_t = 0x4B41_5341_4E45_E701)]
        seed: u64,
        #[arg(long, default_value_t = 0)]
        threads: usize,
        #[arg(long, default_value_t = 800)]
        defender_nodes: u32,
        #[arg(long, default_value_t = 4)]
        base_depth: usize,
        #[arg(long, default_value_t = 64)]
        base_beam_width: usize,
        #[arg(long, default_value_t = 0.42)]
        initial_sigma: f64,
        #[arg(long, default_value = "config/base-model-heuristic-v1.json")]
        source_model: PathBuf,
        #[arg(long, default_value = "models/base-model-evolved-v1.json")]
        output_model: PathBuf,
        #[arg(long, default_value = "results/base-evolution-v1.json")]
        output_report: PathBuf,
    },
    /// Optimize cancellation-dodge and two-ply charging on exact matches.
    EvolveTempo {
        #[arg(long, default_value_t = 12)]
        population: usize,
        #[arg(long, default_value_t = 4)]
        elites: usize,
        #[arg(long, default_value_t = 8)]
        generations: usize,
        #[arg(long, default_value_t = 16)]
        games_per_candidate: usize,
        #[arg(long, default_value_t = 0x4B41_5341_4E45_7E90)]
        seed: u64,
        #[arg(long, default_value_t = 0)]
        threads: usize,
        #[arg(long, default_value_t = 600)]
        defender_nodes: u32,
        #[arg(long, default_value_t = 200)]
        forecast_nodes: u32,
        #[arg(long, default_value_t = 3)]
        base_depth: usize,
        #[arg(long, default_value_t = 32)]
        base_beam_width: usize,
        #[arg(long, default_value_t = 1500)]
        maximum_wait_ms: u64,
        #[arg(long, default_value_t = 0.45)]
        initial_sigma: f64,
        #[arg(long, default_value = "models/base-model-evolved-v3.json")]
        source_base_model: PathBuf,
        #[arg(long, default_value = "config/tempo-model-bootstrap-v3.json")]
        source_tempo_model: PathBuf,
        #[arg(long, default_value = "models/tempo-model-evolved-v1.json")]
        output_model: PathBuf,
        #[arg(long, default_value = "results/tempo-evolution-v1.json")]
        output_report: PathBuf,
    },
}

fn apply_stack_ren_fields(source: &AgentConfig, target: &mut AgentConfig) {
    target.stack_ren_enable = source.stack_ren_enable;
    target.stack_ren_min_well_width = source.stack_ren_min_well_width;
    target.stack_ren_max_well_width = source.stack_ren_max_well_width;
    target.stack_ren_entry_interval = source.stack_ren_entry_interval;
    target.stack_ren_action_limit = source.stack_ren_action_limit;
    target.stack_ren_min_build_pieces = source.stack_ren_min_build_pieces;
    target.stack_ren_max_build_pieces = source.stack_ren_max_build_pieces;
    target.stack_ren_max_build_ms = source.stack_ren_max_build_ms;
    target.stack_ren_target_depth = source.stack_ren_target_depth;
    target.stack_ren_min_fire_chain = source.stack_ren_min_fire_chain;
    target.stack_ren_min_fire_attack = source.stack_ren_min_fire_attack;
    target.stack_ren_min_headroom = source.stack_ren_min_headroom;
    target.stack_ren_max_holes = source.stack_ren_max_holes;
    target.stack_ren_max_due_1000 = source.stack_ren_max_due_1000;
    target.stack_ren_max_forecast_1000 = source.stack_ren_max_forecast_1000;
    target.stack_ren_entry_threshold = source.stack_ren_entry_threshold;
    target.stack_ren_fire_margin = source.stack_ren_fire_margin;
    target.stack_ren_abort_margin = source.stack_ren_abort_margin;
    target.stack_ren_hole_mismatch_penalty = source.stack_ren_hole_mismatch_penalty;
    target.stack_ren_cooldown_pieces = source.stack_ren_cooldown_pieces;
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Command::Benchmark {
            games,
            time_limit_ms,
            seed,
            cold_clear_nodes,
            kasane_nodes,
            forecast_nodes,
            base_depth,
            base_beam_width,
            threads,
            base_model,
            tempo_model,
            strategy_policy,
            stack_ren,
            stack_ren_model,
            disable_tempo,
            enable_charge,
            allow_free_rerank,
            base_guard_margin,
            dodge_wait_cap_ms,
            dodge_finish_height,
            dodge_safety_margin,
            strategy_min_advantage,
            strategy_placement_min_advantage,
            strategy_max_alternatives,
            strategy_max_wait_ms,
            disable_stateful_strategy,
            output,
        } => {
            let mut agent_config = AgentConfig::default();
            let effective_policy = strategy_policy
                .clone()
                .or_else(|| stack_ren.then(|| PathBuf::from("config/kasane-stack-ren-v3.json")));
            if let Some(path) = &effective_policy {
                let text = fs::read_to_string(path)
                    .with_context(|| format!("failed to read policy {}", path.display()))?;
                let policy: StrategyPolicyArtifact = serde_json::from_str(&text)
                    .with_context(|| format!("failed to parse policy {}", path.display()))?;
                policy.apply_to(&mut agent_config);
                if stack_ren {
                    let overlay: AgentConfig = serde_json::from_str(&text).with_context(|| {
                        format!("failed to parse Stack-REN fields from {}", path.display())
                    })?;
                    apply_stack_ren_fields(&overlay, &mut agent_config);
                    let value: serde_json::Value = serde_json::from_str(&text)?;
                    if let Some(model) = value
                        .get("stack_ren_model")
                        .and_then(|value| value.as_str())
                    {
                        agent_config.stack_ren_model_path = Some(PathBuf::from(model));
                    }
                }
            } else {
                agent_config.base_model_path =
                    Some(PathBuf::from("models/base-model-evolved-v3.json"));
                agent_config.tempo_model_path =
                    Some(PathBuf::from("config/tempo-model-bootstrap-v3.json"));
            }
            agent_config.cold_clear_nodes = cold_clear_nodes;
            agent_config.kasane_nodes = kasane_nodes;
            if let Some(value) = forecast_nodes {
                agent_config.forecast_nodes = value;
            }
            agent_config.base_depth = base_depth;
            agent_config.base_beam_width = base_beam_width;
            if base_model.is_some() {
                agent_config.base_model_path = base_model;
            }
            if tempo_model.is_some() {
                agent_config.tempo_model_path = tempo_model;
            }
            if stack_ren_model.is_some() {
                agent_config.stack_ren_model_path = stack_ren_model;
            }
            agent_config.enable_tempo = !disable_tempo;
            agent_config.enable_charge = enable_charge;
            agent_config.strict_base_policy = !stack_ren && !allow_free_rerank;
            agent_config.base_guard_margin = base_guard_margin;
            agent_config.dodge_wait_cap_ms = dodge_wait_cap_ms;
            agent_config.dodge_finish_height = dodge_finish_height;
            agent_config.dodge_safety_margin = dodge_safety_margin;
            if let Some(value) = strategy_min_advantage {
                agent_config.strategy_min_advantage = value;
            }
            if let Some(value) = strategy_placement_min_advantage {
                agent_config.strategy_placement_min_advantage = value;
            }
            if let Some(value) = strategy_max_alternatives {
                agent_config.strategy_max_alternatives = value;
            }
            if let Some(value) = strategy_max_wait_ms {
                agent_config.strategy_max_wait_ms = value;
            }
            if disable_stateful_strategy {
                agent_config.strategy_enable_stateful = false;
            }
            let config = CheeseBenchmarkConfig {
                games,
                seed,
                time_limit_ms,
                threads,
                kasane_agent: if stack_ren {
                    AgentKind::KasaneStackRen
                } else {
                    AgentKind::Kasane
                },
                agent_config,
                ..CheeseBenchmarkConfig::default()
            };
            let report = run_cheese_benchmark(config)?;
            let json = serde_json::to_string_pretty(&report)?;
            if let Some(path) = output {
                if let Some(parent) = path.parent() {
                    fs::create_dir_all(parent).with_context(|| {
                        format!("failed to create output directory {}", parent.display())
                    })?;
                }
                fs::write(&path, json.as_bytes())
                    .with_context(|| format!("failed to write {}", path.display()))?;
            }
            println!("{json}");
        }
        Command::ExportDefaultModel { output } => {
            if let Some(parent) = output.parent() {
                fs::create_dir_all(parent)?;
            }
            TempoModel::default().save(&output)?;
            println!("wrote {}", output.display());
        }
        Command::ExportDefaultBaseModel { output } => {
            if let Some(parent) = output.parent() {
                fs::create_dir_all(parent)?;
            }
            BaseModel::default().save(&output)?;
            println!("wrote {}", output.display());
        }
        Command::GenerateBaseDataset {
            states,
            episode_length,
            teacher_nodes,
            seed,
            threads,
            output,
            behavior_model,
            behavior_probability,
        } => {
            let report = generate_base_dataset(BaseDatasetConfig {
                states,
                episode_length,
                teacher_nodes,
                seed,
                threads,
                output,
                behavior_model_path: behavior_model,
                behavior_probability,
            })?;
            println!("{}", serde_json::to_string_pretty(&report)?);
        }
        Command::EvolveBase {
            population,
            elites,
            generations,
            games_per_candidate,
            seed,
            threads,
            defender_nodes,
            base_depth,
            base_beam_width,
            initial_sigma,
            source_model,
            output_model,
            output_report,
        } => {
            let report = evolve_base(EvolutionConfig {
                population,
                elites,
                generations,
                games_per_candidate,
                seed,
                threads,
                defender_nodes,
                base_depth,
                base_beam_width,
                initial_sigma,
                source_model,
                output_model,
                output_report,
                ..EvolutionConfig::default()
            })?;
            println!("{}", serde_json::to_string_pretty(&report.best)?);
        }
        Command::EvolveTempo {
            population,
            elites,
            generations,
            games_per_candidate,
            seed,
            threads,
            defender_nodes,
            forecast_nodes,
            base_depth,
            base_beam_width,
            maximum_wait_ms,
            initial_sigma,
            source_base_model,
            source_tempo_model,
            output_model,
            output_report,
        } => {
            let report = evolve_tempo(TempoEvolutionConfig {
                population,
                elites,
                generations,
                games_per_candidate,
                seed,
                threads,
                defender_nodes,
                forecast_nodes,
                base_depth,
                base_beam_width,
                maximum_wait_ms,
                initial_sigma,
                source_base_model,
                source_tempo_model,
                output_model,
                output_report,
                ..TempoEvolutionConfig::default()
            })?;
            println!("{}", serde_json::to_string_pretty(&report.best)?);
        }
    }
    Ok(())
}
