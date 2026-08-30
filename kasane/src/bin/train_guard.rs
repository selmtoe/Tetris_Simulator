use anyhow::Result;
use clap::Parser;
use std::path::PathBuf;

#[allow(dead_code)]
#[path = "../guard_training.rs"]
mod guard_training;

use guard_training::{run_guard_training, GuardTrainingConfig};

#[derive(Parser, Debug)]
#[command(
    name = "train_guard",
    version,
    about = "Train, validation-select and final-holdout-evaluate KASANE Guard"
)]
struct Cli {
    #[arg(long, default_value_t = 12)]
    population: usize,
    #[arg(long, default_value_t = 4)]
    elites: usize,
    #[arg(long, default_value_t = 8)]
    generations: usize,
    #[arg(long, default_value_t = 64)]
    train_games: usize,
    #[arg(long, default_value_t = 64)]
    validation_games: usize,
    #[arg(long, default_value_t = 256)]
    final_holdout_games: usize,
    #[arg(long, default_value_t = 0x4B41_5341_4E45_4701)]
    train_seed: u64,
    #[arg(long, default_value_t = 0x4B41_5341_4E45_4801)]
    validation_seed: u64,
    #[arg(long, default_value_t = 0x4B41_5341_4E45_4A01)]
    final_holdout_seed: u64,
    #[arg(long, default_value_t = 0)]
    threads: usize,
    #[arg(long, default_value_t = 20_000)]
    time_limit_ms: u64,
    #[arg(long, default_value_t = 600)]
    attacker_nodes: u32,
    #[arg(long, default_value_t = 600)]
    defender_nodes: u32,
    #[arg(long, default_value_t = 200)]
    forecast_nodes: u32,
    #[arg(long, default_value_t = 3)]
    base_depth: usize,
    #[arg(long, default_value_t = 32)]
    base_beam_width: usize,
    #[arg(long, value_delimiter = ',', default_value = "10,14,16")]
    defender_cheese_rows: Vec<u32>,
    #[arg(long, default_value_t = 4)]
    danger_headroom: i32,
    #[arg(long, default_value_t = 0.18)]
    initial_sigma: f64,
    #[arg(long, default_value_t = 0.03)]
    sigma_floor: f64,
    #[arg(long, default_value = "models/base-model-evolved-v3.json")]
    source_base_model: PathBuf,
    #[arg(long, default_value = "config/tempo-model-bootstrap-v3.json")]
    source_tempo_model: PathBuf,
    #[arg(long, default_value = "models/guard-model-evolved-v1.json")]
    output_model: PathBuf,
    #[arg(long, default_value = "config/kasane-guard-v1.json")]
    output_policy: PathBuf,
    #[arg(long, default_value = "results/guard-training-v1.json")]
    output_report: PathBuf,
    /// Print the complete JSON report to stdout in addition to writing it.
    #[arg(long, default_value_t = false)]
    print_report: bool,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let print_report = cli.print_report;
    let output_report = cli.output_report.clone();
    let report = run_guard_training(GuardTrainingConfig {
        population: cli.population,
        elites: cli.elites,
        generations: cli.generations,
        train_games: cli.train_games,
        validation_games: cli.validation_games,
        final_holdout_games: cli.final_holdout_games,
        train_seed: cli.train_seed,
        validation_seed: cli.validation_seed,
        final_holdout_seed: cli.final_holdout_seed,
        threads: cli.threads,
        time_limit_ms: cli.time_limit_ms,
        attacker_nodes: cli.attacker_nodes,
        defender_nodes: cli.defender_nodes,
        forecast_nodes: cli.forecast_nodes,
        base_depth: cli.base_depth,
        base_beam_width: cli.base_beam_width,
        defender_cheese_rows: cli.defender_cheese_rows,
        danger_headroom: cli.danger_headroom,
        initial_sigma: cli.initial_sigma,
        sigma_floor: cli.sigma_floor,
        source_base_model: cli.source_base_model,
        source_tempo_model: cli.source_tempo_model,
        output_model: cli.output_model,
        output_policy: cli.output_policy,
        output_report: cli.output_report,
        ..GuardTrainingConfig::default()
    })?;
    if print_report {
        println!("{}", serde_json::to_string_pretty(&report)?);
    } else {
        println!(
            "train: CC {}/{} vs Guard {}/{}; validation: CC {}/{} vs Guard {}/{}; final_holdout: CC {}/{} vs Guard {}/{}; report={}",
            report.train.cold_clear.topouts,
            report.train.games,
            report.train.guard.topouts,
            report.train.games,
            report.validation.cold_clear.topouts,
            report.validation.games,
            report.validation.guard.topouts,
            report.validation.games,
            report.final_holdout.cold_clear.topouts,
            report.final_holdout.games,
            report.final_holdout.guard.topouts,
            report.final_holdout.games,
            output_report.display(),
        );
    }
    Ok(())
}
