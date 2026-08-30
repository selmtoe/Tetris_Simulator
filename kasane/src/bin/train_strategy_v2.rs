use anyhow::Result;
use clap::Parser;
use kasane::strategy_training::{train_strategy_v2, StrategyTrainingConfig};
use std::path::PathBuf;

#[derive(Parser)]
#[command(
    name = "train_strategy_v2",
    about = "Train the equal-node Cold Clear floor KASANE v2 strategy with paired CEM"
)]
struct Cli {
    #[arg(long, default_value_t = 10)]
    population: usize,
    #[arg(long, default_value_t = 3)]
    elites: usize,
    #[arg(long, default_value_t = 5)]
    generations: usize,
    #[arg(long, default_value_t = 10)]
    train_games: usize,
    #[arg(long, default_value_t = 12)]
    validation_games: usize,
    #[arg(long, default_value_t = 40)]
    holdout_games: usize,
    #[arg(long, default_value_t = 0x4B41_5341_4E45_7201)]
    train_seed: u64,
    #[arg(long, default_value_t = 0x4B41_5341_4E45_7202)]
    validation_seed: u64,
    #[arg(long, default_value_t = 0x4B41_5341_4E45_7203)]
    holdout_seed: u64,
    #[arg(long, default_value_t = 400)]
    nodes: u32,
    #[arg(long, default_value_t = 160)]
    forecast_nodes: u32,
    #[arg(long, default_value_t = 20_000)]
    time_limit_ms: u64,
    #[arg(long, default_value_t = 0)]
    threads: usize,
    #[arg(long, default_value_t = 0.35)]
    initial_sigma: f64,
    #[arg(long, default_value_t = 0.06)]
    sigma_floor: f64,
    #[arg(long, default_value = "models/base-model-evolved-v3.json")]
    source_base_model: PathBuf,
    #[arg(long)]
    source_strategy_model: Option<PathBuf>,
    #[arg(long, default_value = "models/strategy-model-v2.json")]
    output_model: PathBuf,
    #[arg(long, default_value = "config/kasane-strategy-v2.json")]
    output_policy: PathBuf,
    #[arg(long, default_value = "results/strategy-training-v2.json")]
    output_report: PathBuf,
    #[arg(long, default_value_t = 16)]
    strategy_max_alternatives: usize,
    #[arg(long, default_value_t = 1_000)]
    strategy_max_wait_ms: u64,
    #[arg(long, default_value_t = 0.35)]
    initial_min_advantage: f32,
    #[arg(long, default_value_t = 3.0)]
    placement_min_advantage: f32,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let report = train_strategy_v2(StrategyTrainingConfig {
        population: cli.population,
        elites: cli.elites,
        generations: cli.generations,
        train_games: cli.train_games,
        validation_games: cli.validation_games,
        holdout_games: cli.holdout_games,
        train_seed: cli.train_seed,
        validation_seed: cli.validation_seed,
        holdout_seed: cli.holdout_seed,
        nodes: cli.nodes,
        forecast_nodes: cli.forecast_nodes,
        time_limit_ms: cli.time_limit_ms,
        threads: cli.threads,
        initial_sigma: cli.initial_sigma,
        sigma_floor: cli.sigma_floor,
        source_base_model: cli.source_base_model,
        source_strategy_model: cli.source_strategy_model,
        output_model: cli.output_model,
        output_policy: cli.output_policy,
        output_report: cli.output_report,
        strategy_max_alternatives: cli.strategy_max_alternatives,
        strategy_max_wait_ms: cli.strategy_max_wait_ms,
        initial_min_advantage: cli.initial_min_advantage,
        placement_min_advantage: cli.placement_min_advantage,
    })?;
    println!("{}", serde_json::to_string_pretty(&report.holdout)?);
    Ok(())
}
