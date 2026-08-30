use crate::agent::{AgentConfig, AgentKind};
use crate::game::{
    InitialField, Match, MatchConfig, MatchOutcome, MatchResult, PlayerSpec, PlayerStats,
};
use crate::rules::Rules;
use anyhow::{bail, Result};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct CheeseBenchmarkConfig {
    pub games: usize,
    pub seed: u64,
    pub time_limit_ms: u64,
    pub horizons_ms: Vec<u64>,
    pub threads: usize,
    pub agent_config: AgentConfig,
}

impl Default for CheeseBenchmarkConfig {
    fn default() -> Self {
        Self {
            games: 100,
            seed: 0x4B41_5341_4E45_0001,
            time_limit_ms: 30_000,
            horizons_ms: vec![5_000, 10_000, 15_000, 20_000, 30_000],
            threads: 0,
            agent_config: AgentConfig::default(),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct HorizonRate {
    pub horizon_ms: u64,
    pub successes: usize,
    pub probability: f64,
    pub wilson_low: f64,
    pub wilson_high: f64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct RunSummary {
    pub attacker: AgentKind,
    pub defender: AgentKind,
    pub games: usize,
    pub attacker_wins: usize,
    pub attacker_losses: usize,
    pub draws_or_timeouts: usize,
    pub ko_cdf: Vec<HorizonRate>,
    pub mean_pieces: f64,
    pub mean_raw_attack: f64,
    pub mean_attack_sent: f64,
    pub mean_attack_cancelled: f64,
    pub mean_waited_ms: f64,
    pub mean_max_combo: f64,
    pub mean_cancellation_dodges: f64,
    pub mean_tank_actions: f64,
    pub intent_totals: BTreeMap<String, u64>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PairedComparison {
    pub primary_horizon_ms: u64,
    pub cold_clear_probability: f64,
    pub kasane_probability: f64,
    pub difference_percentage_points: f64,
    pub paired_ci95_low_pp: f64,
    pub paired_ci95_high_pp: f64,
    pub kasane_only_wins: usize,
    pub cold_clear_only_wins: usize,
    pub target_difference_pp: f64,
    pub target_lower_ci_pp: f64,
    pub passed_ambitious_target: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CheeseBenchmarkReport {
    pub schema: String,
    pub rules: Rules,
    pub scenario: String,
    pub config: CheeseBenchmarkConfig,
    pub cold_clear: RunSummary,
    pub kasane: RunSummary,
    pub paired: PairedComparison,
}

struct PairResult {
    cold_clear: MatchResult,
    kasane: MatchResult,
}

pub fn run_cheese_benchmark(config: CheeseBenchmarkConfig) -> Result<CheeseBenchmarkReport> {
    if config.games == 0 {
        bail!("benchmark requires at least one game");
    }
    if !config.horizons_ms.contains(&20_000) {
        bail!("benchmark horizons must contain the primary 20000 ms point");
    }
    let run = || {
        (0..config.games)
            .into_par_iter()
            .map(|index| {
                let seed = benchmark_seed(config.seed, index as u64);
                Ok(PairResult {
                    cold_clear: run_scenario(seed, AgentKind::ColdClear, &config)?,
                    kasane: run_scenario(seed, AgentKind::Kasane, &config)?,
                })
            })
            .collect::<Result<Vec<_>>>()
    };
    let pairs = if config.threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(config.threads)
            .build()?
            .install(run)?
    } else {
        run()?
    };

    let cold_results: Vec<_> = pairs.iter().map(|pair| &pair.cold_clear).collect();
    let kasane_results: Vec<_> = pairs.iter().map(|pair| &pair.kasane).collect();
    let cold_clear = summarize(AgentKind::ColdClear, &cold_results, &config.horizons_ms);
    let kasane = summarize(AgentKind::Kasane, &kasane_results, &config.horizons_ms);
    let paired = paired_comparison(&pairs, 20_000);
    Ok(CheeseBenchmarkReport {
        schema: "kasane-cheese12-benchmark/v1".to_owned(),
        rules: Rules::pinned(),
        scenario: "attacker empty vs Cold Clear defender on bottom 12 strict random-hole rows; PC special attack/reward zero"
            .to_owned(),
        config,
        cold_clear,
        kasane,
        paired,
    })
}

fn run_scenario(
    seed: u64,
    attacker: AgentKind,
    benchmark: &CheeseBenchmarkConfig,
) -> Result<MatchResult> {
    let config = MatchConfig {
        rules: Rules::pinned(),
        players: [
            PlayerSpec {
                agent: attacker,
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
        agent_configs: [
            benchmark.agent_config.clone(),
            benchmark.agent_config.clone(),
        ],
        seed,
        time_limit_ms: benchmark.time_limit_ms,
        record_trace: false,
    };
    Match::new(config)?.run()
}

fn summarize(attacker: AgentKind, results: &[&MatchResult], horizons: &[u64]) -> RunSummary {
    let games = results.len();
    let attacker_wins = results
        .iter()
        .filter(|result| result.outcome == MatchOutcome::Player0Win)
        .count();
    let attacker_losses = results
        .iter()
        .filter(|result| result.outcome == MatchOutcome::Player1Win)
        .count();
    let totals = results
        .iter()
        .fold(PlayerStats::default(), |mut sum, result| {
            sum.pieces += result.stats[0].pieces;
            sum.raw_attack += result.stats[0].raw_attack;
            sum.sent += result.stats[0].sent;
            sum.cancelled += result.stats[0].cancelled;
            sum.waited_ms += result.stats[0].waited_ms;
            sum.max_combo += result.stats[0].max_combo;
            sum.cancellation_dodges += result.stats[0].cancellation_dodges;
            sum.tank_actions += result.stats[0].tank_actions;
            for (intent, count) in &result.stats[0].intents {
                *sum.intents.entry(intent.clone()).or_default() += count;
            }
            sum
        });
    RunSummary {
        attacker,
        defender: AgentKind::ColdClear,
        games,
        attacker_wins,
        attacker_losses,
        draws_or_timeouts: games - attacker_wins - attacker_losses,
        ko_cdf: horizons
            .iter()
            .map(|&horizon| {
                let successes = results
                    .iter()
                    .filter(|result| success_by(result, horizon))
                    .count();
                let (low, high) = wilson(successes, games);
                HorizonRate {
                    horizon_ms: horizon,
                    successes,
                    probability: successes as f64 / games as f64,
                    wilson_low: low,
                    wilson_high: high,
                }
            })
            .collect(),
        mean_pieces: totals.pieces as f64 / games as f64,
        mean_raw_attack: totals.raw_attack as f64 / games as f64,
        mean_attack_sent: totals.sent as f64 / games as f64,
        mean_attack_cancelled: totals.cancelled as f64 / games as f64,
        mean_waited_ms: totals.waited_ms as f64 / games as f64,
        mean_max_combo: totals.max_combo as f64 / games as f64,
        mean_cancellation_dodges: totals.cancellation_dodges as f64 / games as f64,
        mean_tank_actions: totals.tank_actions as f64 / games as f64,
        intent_totals: totals.intents,
    }
}

fn paired_comparison(pairs: &[PairResult], horizon: u64) -> PairedComparison {
    let values: Vec<f64> = pairs
        .iter()
        .map(|pair| {
            success_by(&pair.kasane, horizon) as u8 as f64
                - success_by(&pair.cold_clear, horizon) as u8 as f64
        })
        .collect();
    let n = values.len() as f64;
    let mean = values.iter().sum::<f64>() / n;
    let variance = if values.len() > 1 {
        values
            .iter()
            .map(|value| (value - mean).powi(2))
            .sum::<f64>()
            / (n - 1.0)
    } else {
        0.0
    };
    let half_width = 1.96 * (variance / n).sqrt();
    let kasane_only = values.iter().filter(|&&value| value > 0.0).count();
    let cold_only = values.iter().filter(|&&value| value < 0.0).count();
    let cold_probability = pairs
        .iter()
        .filter(|pair| success_by(&pair.cold_clear, horizon))
        .count() as f64
        / n;
    let kasane_probability = pairs
        .iter()
        .filter(|pair| success_by(&pair.kasane, horizon))
        .count() as f64
        / n;
    PairedComparison {
        primary_horizon_ms: horizon,
        cold_clear_probability: cold_probability,
        kasane_probability,
        difference_percentage_points: mean * 100.0,
        paired_ci95_low_pp: (mean - half_width) * 100.0,
        paired_ci95_high_pp: (mean + half_width) * 100.0,
        kasane_only_wins: kasane_only,
        cold_clear_only_wins: cold_only,
        target_difference_pp: 15.0,
        target_lower_ci_pp: 10.0,
        passed_ambitious_target: mean >= 0.15 && mean - half_width >= 0.10,
    }
}

fn success_by(result: &MatchResult, horizon: u64) -> bool {
    result.outcome == MatchOutcome::Player0Win && result.ended_ms <= horizon
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
    (center - radius, center + radius)
}

fn benchmark_seed(seed: u64, index: u64) -> u64 {
    let mut value = seed.wrapping_add(index.wrapping_mul(0x9E37_79B9_7F4A_7C15));
    value = (value ^ (value >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    value ^ (value >> 31)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wilson_interval_contains_observed_rate() {
        let (low, high) = wilson(7, 10);
        assert!(low < 0.7 && high > 0.7);
    }
}
