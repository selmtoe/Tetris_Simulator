use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use kasane::agent::AgentConfig;
use kasane::base::BaseModel;
use kasane::benchmark::{run_cheese_benchmark, CheeseBenchmarkConfig};
use kasane::evolution::{evolve_base, evolve_tempo, EvolutionConfig, TempoEvolutionConfig};
use kasane::model::TempoModel;
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
        #[arg(long, default_value_t = 1_500)]
        kasane_nodes: u32,
        #[arg(long, default_value_t = 300)]
        forecast_nodes: u32,
        #[arg(long, default_value_t = 4)]
        base_depth: usize,
        #[arg(long, default_value_t = 64)]
        base_beam_width: usize,
        #[arg(long, default_value_t = 0)]
        threads: usize,
        #[arg(long, default_value = "models/base-model-evolved-v3.json")]
        base_model: Option<PathBuf>,
        #[arg(long, default_value = "config/tempo-model-bootstrap-v3.json")]
        tempo_model: Option<PathBuf>,
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
            disable_tempo,
            enable_charge,
            allow_free_rerank,
            base_guard_margin,
            dodge_wait_cap_ms,
            dodge_finish_height,
            dodge_safety_margin,
            output,
        } => {
            let config = CheeseBenchmarkConfig {
                games,
                seed,
                time_limit_ms,
                threads,
                agent_config: AgentConfig {
                    cold_clear_nodes,
                    kasane_nodes,
                    forecast_nodes,
                    base_depth,
                    base_beam_width,
                    base_model_path: base_model,
                    tempo_model_path: tempo_model,
                    enable_tempo: !disable_tempo,
                    enable_charge,
                    strict_base_policy: !allow_free_rerank,
                    base_guard_margin,
                    dodge_wait_cap_ms,
                    dodge_finish_height,
                    dodge_safety_margin,
                    ..AgentConfig::default()
                },
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
