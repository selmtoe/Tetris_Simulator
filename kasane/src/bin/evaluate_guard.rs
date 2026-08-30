use anyhow::Result;
use clap::Parser;
use std::path::PathBuf;

#[allow(dead_code)]
#[path = "../guard_training.rs"]
mod guard_training;

use guard_training::{run_guard_evaluation, GuardEvaluationConfig};

#[derive(Parser, Debug)]
#[command(
    name = "evaluate_guard",
    version,
    about = "Evaluate a frozen KASANE Guard policy on a paired unseen seed root"
)]
struct Cli {
    #[arg(long, default_value_t = 256)]
    games: usize,
    #[arg(long, default_value_t = 0x4B41_5341_4E45_5001)]
    seed: u64,
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
    #[arg(long, default_value = "config/kasane-guard-v1.json")]
    policy: PathBuf,
    #[arg(long, default_value = "results/guard-holdout-strong-v1.json")]
    output: PathBuf,
    #[arg(long, default_value_t = false)]
    print_report: bool,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let output = cli.output.clone();
    let report = run_guard_evaluation(GuardEvaluationConfig {
        games: cli.games,
        seed: cli.seed,
        threads: cli.threads,
        time_limit_ms: cli.time_limit_ms,
        attacker_nodes: cli.attacker_nodes,
        defender_nodes: cli.defender_nodes,
        forecast_nodes: cli.forecast_nodes,
        base_depth: cli.base_depth,
        base_beam_width: cli.base_beam_width,
        defender_cheese_rows: cli.defender_cheese_rows,
        danger_headroom: cli.danger_headroom,
        policy_path: cli.policy,
        output_report: cli.output,
        ..GuardEvaluationConfig::default()
    })?;
    if cli.print_report {
        println!("{}", serde_json::to_string_pretty(&report)?);
    } else {
        println!(
            "frozen strong holdout: CC {}/{} topouts vs Guard {}/{}; reduction={:.3}pp; digest_equal={}; report={}",
            report.evaluation.cold_clear.topouts,
            report.evaluation.games,
            report.evaluation.guard.topouts,
            report.evaluation.games,
            report.evaluation.paired.topout_reduction_percentage_points,
            report.evaluation.attack_schedules_identical,
            output.display(),
        );
    }
    Ok(())
}
