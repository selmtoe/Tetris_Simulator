use crate::agent::{AgentConfig, AgentKind};
use crate::game::{
    InitialField, Match, MatchConfig, MatchOutcome, MatchResult, PlayerSpec, PlayerStats,
};
use crate::rules::Rules;
use anyhow::{bail, Result};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct CheeseBenchmarkConfig {
    pub games: usize,
    pub seed: u64,
    pub time_limit_ms: u64,
    pub horizons_ms: Vec<u64>,
    pub threads: usize,
    pub kasane_agent: AgentKind,
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
            kasane_agent: AgentKind::Kasane,
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
    pub mean_attack_expert_actions: f64,
    pub mean_survival_expert_actions: f64,
    pub mean_attack_expert_overrides: f64,
    pub mean_survival_expert_overrides: f64,
    pub mean_hold_fire_actions: f64,
    pub mean_hold_fire_raw_attack: f64,
    pub mean_hold_fire_sent: f64,
    pub mean_charge_release_incoming_edges: f64,
    pub mean_charge_release_garbage_rise_edges: f64,
    pub mean_charge_release_raw_attack: f64,
    pub mean_charge_release_sent: f64,
    pub mean_ren_starts: f64,
    pub mean_ren_continuations: f64,
    pub mean_stack_ren_entries: f64,
    pub mean_stack_ren_build_actions: f64,
    pub mean_stack_ren_fires: f64,
    pub mean_stack_ren_continuations: f64,
    pub mean_stack_ren_aborts: f64,
    pub mean_stack_ren_fire_sent: f64,
    pub intent_totals: BTreeMap<String, u64>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PairedComparison {
    pub primary_horizon_ms: u64,
    pub cold_clear_probability: f64,
    pub kasane_probability: f64,
    pub difference_percentage_points: f64,
    /// Exploratory normal/Wald interval retained for report compatibility.
    /// It is not used for the machine-readable superiority decision because it
    /// can collapse to zero width when every paired observation is identical.
    pub paired_ci95_low_pp: f64,
    pub paired_ci95_high_pp: f64,
    /// Distribution-free, fixed-sample two-sided Hoeffding confidence
    /// interval for the mean paired difference. Each seed contributes exactly
    /// one value in [-1, 1].
    #[serde(default)]
    pub fixed_sample_hoeffding_ci95_low_pp: f64,
    #[serde(default)]
    pub fixed_sample_hoeffding_ci95_high_pp: f64,
    pub kasane_only_wins: usize,
    pub cold_clear_only_wins: usize,
    pub target_difference_pp: f64,
    pub target_lower_ci_pp: f64,
    /// Legacy exploratory target decision based on the normal/Wald interval.
    /// Kept so existing report consumers remain compatible.
    pub passed_ambitious_target: bool,
    #[serde(default)]
    pub exploratory_interval_method: String,
    #[serde(default)]
    pub superiority_method: String,
    #[serde(default)]
    pub superior_to_cold_clear: bool,
    #[serde(default)]
    pub passed_ambitious_target_fixed_sample: bool,
    /// Exact one-sided McNemar/binomial tail over discordant fixed pairs.
    /// This p-value is valid only for a sample size fixed before observing
    /// outcomes; repeatedly checking it and stopping early is not allowed.
    #[serde(default = "default_exact_p_value")]
    pub exact_one_sided_mcnemar_p_value: f64,
    #[serde(default)]
    pub exact_one_sided_mcnemar_method: String,
    #[serde(default)]
    pub exact_one_sided_mcnemar_rejects_equal_20s_cheese_success_at_alpha_0_05: bool,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct ModelArtifactProvenance {
    pub role: String,
    pub path: String,
    pub sha256: Option<String>,
    pub read_error: Option<String>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct BenchmarkProvenance {
    /// Upstream Cold Clear 1 source snapshot vendored by this repository.
    pub cold_clear_upstream_revision: String,
    pub cold_clear_evaluator: String,
    pub cold_clear_runtime_mode: String,
    pub deterministic_search_mode: String,
    pub pair_collection_mode: String,
    /// SHA-256 of the fully resolved, serializable AgentConfig. In-memory
    /// model overrides are skipped by AgentConfig's serialization and are
    /// separately flagged below.
    pub resolved_agent_config_sha256: String,
    pub has_in_memory_model_overrides: bool,
    /// Hashes of the local Cold Clear source files actually included by the
    /// simulator build, independent of the declared upstream revision.
    #[serde(default)]
    pub cold_clear_source_artifacts: Vec<ModelArtifactProvenance>,
    /// Hash of the executable that is running this league. This is the
    /// authoritative compiled-code identifier; source hashes are explanatory.
    #[serde(default)]
    pub running_executable: ModelArtifactProvenance,
    /// Content hashes of model files that are named by the resolved config.
    /// The outer policy file cannot be hashed here because its path is consumed
    /// before CheeseBenchmarkConfig is constructed.
    pub model_artifacts: Vec<ModelArtifactProvenance>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CheeseBenchmarkReport {
    pub schema: String,
    pub rules: Rules,
    pub scenario: String,
    pub config: CheeseBenchmarkConfig,
    #[serde(default)]
    pub provenance: BenchmarkProvenance,
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
    // Snapshot provenance before any worker loads a model so the recorded
    // content hashes describe the artifacts visible at benchmark start.
    let provenance = benchmark_provenance(&config)?;
    let run = || {
        (0..config.games)
            .into_par_iter()
            .map(|index| {
                let seed = benchmark_seed(config.seed, index as u64);
                Ok(PairResult {
                    cold_clear: run_scenario(seed, AgentKind::ColdClear, &config)?,
                    kasane: run_scenario(seed, config.kasane_agent, &config)?,
                })
            })
            // `0..games` is indexed, so collecting every slot into a Vec
            // preserves seed order. Transpose Result sequentially below;
            // Result's parallel short-circuit collector need not document
            // which error/partial ordering is observed.
            .collect::<Vec<Result<PairResult>>>()
    };
    let pair_results = if config.threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(config.threads)
            .build()?
            .install(run)
    } else {
        run()
    };
    let pairs = pair_results.into_iter().collect::<Result<Vec<_>>>()?;

    let cold_results: Vec<_> = pairs.iter().map(|pair| &pair.cold_clear).collect();
    let kasane_results: Vec<_> = pairs.iter().map(|pair| &pair.kasane).collect();
    let cold_clear = summarize(AgentKind::ColdClear, &cold_results, &config.horizons_ms);
    let kasane = summarize(config.kasane_agent, &kasane_results, &config.horizons_ms);
    let paired = paired_comparison(&pairs, 20_000);
    Ok(CheeseBenchmarkReport {
        schema: "kasane-cheese12-benchmark/v5-fixed-sample-provenance".to_owned(),
        rules: Rules::pinned(),
        scenario: "attacker empty vs Cold Clear defender on bottom 12 strict random-hole rows; PC special attack/reward zero"
            .to_owned(),
        config,
        provenance,
        cold_clear,
        kasane,
        paired,
    })
}

fn benchmark_provenance(config: &CheeseBenchmarkConfig) -> Result<BenchmarkProvenance> {
    let serialized_config = serde_json::to_vec(&config.agent_config)?;
    let cold_clear_root =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../third_party/cold-clear-reference/bot/src");
    let cold_clear_source_artifacts = vec![
        file_artifact_provenance("cold_clear_dag_source", &cold_clear_root.join("dag.rs")),
        file_artifact_provenance(
            "cold_clear_standard_evaluator_source",
            &cold_clear_root.join("evaluation/standard.rs"),
        ),
    ];
    let mut model_artifacts = Vec::new();
    if let Some(path) = &config.agent_config.base_model_path {
        model_artifacts.push(file_artifact_provenance("base_model", path));
    }
    if let Some(path) = &config.agent_config.tempo_model_path {
        model_artifacts.push(file_artifact_provenance("tempo_model", path));
    }
    if config.kasane_agent == AgentKind::KasaneStackRen {
        if let Some(path) = &config.agent_config.stack_ren_model_path {
            model_artifacts.push(file_artifact_provenance("stack_ren_model", path));
        }
    }
    let running_executable = match std::env::current_exe() {
        Ok(path) => file_artifact_provenance("running_executable", &path),
        Err(error) => ModelArtifactProvenance {
            role: "running_executable".to_owned(),
            path: String::new(),
            sha256: None,
            read_error: Some(error.to_string()),
        },
    };
    Ok(BenchmarkProvenance {
        cold_clear_upstream_revision: "279edd7c3177ff8077f6a930193397814b281f27".to_owned(),
        cold_clear_evaluator: "Cold Clear 1 Standard evaluator".to_owned(),
        cold_clear_runtime_mode:
            "native persistent generation-aware DAG per player; opponent forecasts are stateless"
                .to_owned(),
        deterministic_search_mode:
            "thread-local RNG reseeded from board, node budget, and incoming before each bounded think"
                .to_owned(),
        pair_collection_mode:
            "fixed indexed seed schedule; parallel Vec<Result> collection followed by sequential transpose"
                .to_owned(),
        resolved_agent_config_sha256: sha256_hex(&serialized_config),
        has_in_memory_model_overrides: config.agent_config.base_model_override.is_some()
            || config.agent_config.tempo_model_override.is_some()
            || config.agent_config.stack_ren_model_override.is_some(),
        cold_clear_source_artifacts,
        running_executable,
        model_artifacts,
    })
}

fn file_artifact_provenance(role: &str, path: &Path) -> ModelArtifactProvenance {
    match fs::read(path) {
        Ok(content) => ModelArtifactProvenance {
            role: role.to_owned(),
            path: path.display().to_string(),
            sha256: Some(sha256_hex(&content)),
            read_error: None,
        },
        Err(error) => ModelArtifactProvenance {
            role: role.to_owned(),
            path: path.display().to_string(),
            sha256: None,
            read_error: Some(error.to_string()),
        },
    }
}

fn run_scenario(
    seed: u64,
    attacker: AgentKind,
    benchmark: &CheeseBenchmarkConfig,
) -> Result<MatchResult> {
    let mut attacker_config = benchmark.agent_config.clone();
    attacker_config.cold_clear_nodes = if attacker == AgentKind::ColdClear {
        benchmark.agent_config.cold_clear_nodes
    } else {
        benchmark.agent_config.kasane_nodes
    };
    let mut defender_config = benchmark.agent_config.clone();
    defender_config.cold_clear_nodes = benchmark.agent_config.cold_clear_nodes;
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
        agent_configs: [attacker_config, defender_config],
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
            sum.policy_overrides += result.stats[0].policy_overrides;
            sum.placement_overrides += result.stats[0].placement_overrides;
            sum.wait_overrides += result.stats[0].wait_overrides;
            sum.dodge_overrides += result.stats[0].dodge_overrides;
            sum.counter_overrides += result.stats[0].counter_overrides;
            sum.charge_entries += result.stats[0].charge_entries;
            sum.charge_actions += result.stats[0].charge_actions;
            sum.armed_actions += result.stats[0].armed_actions;
            sum.releases += result.stats[0].releases;
            sum.release_dodges += result.stats[0].release_dodges;
            sum.release_counters += result.stats[0].release_counters;
            sum.release_immediate += result.stats[0].release_immediate;
            sum.charge_aborts += result.stats[0].charge_aborts;
            sum.charge_overrides += result.stats[0].charge_overrides;
            sum.release_overrides += result.stats[0].release_overrides;
            sum.attack_expert_actions += result.stats[0].attack_expert_actions;
            sum.survival_expert_actions += result.stats[0].survival_expert_actions;
            sum.attack_expert_overrides += result.stats[0].attack_expert_overrides;
            sum.survival_expert_overrides += result.stats[0].survival_expert_overrides;
            sum.hold_fire_actions += result.stats[0].hold_fire_actions;
            sum.hold_fire_raw_attack += result.stats[0].hold_fire_raw_attack;
            sum.hold_fire_sent += result.stats[0].hold_fire_sent;
            sum.charge_release_incoming_edges += result.stats[0].charge_release_incoming_edges;
            sum.charge_release_garbage_rise_edges +=
                result.stats[0].charge_release_garbage_rise_edges;
            sum.charge_release_raw_attack += result.stats[0].charge_release_raw_attack;
            sum.charge_release_sent += result.stats[0].charge_release_sent;
            sum.ren_starts += result.stats[0].ren_starts;
            sum.ren_continuations += result.stats[0].ren_continuations;
            sum.stack_ren_entries += result.stats[0].stack_ren_entries;
            sum.stack_ren_build_actions += result.stats[0].stack_ren_build_actions;
            sum.stack_ren_fires += result.stats[0].stack_ren_fires;
            sum.stack_ren_continuations += result.stats[0].stack_ren_continuations;
            sum.stack_ren_aborts += result.stats[0].stack_ren_aborts;
            sum.stack_ren_fire_sent += result.stats[0].stack_ren_fire_sent;
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
        mean_policy_overrides: totals.policy_overrides as f64 / games as f64,
        mean_placement_overrides: totals.placement_overrides as f64 / games as f64,
        mean_wait_overrides: totals.wait_overrides as f64 / games as f64,
        mean_dodge_overrides: totals.dodge_overrides as f64 / games as f64,
        mean_counter_overrides: totals.counter_overrides as f64 / games as f64,
        mean_charge_entries: totals.charge_entries as f64 / games as f64,
        mean_charge_actions: totals.charge_actions as f64 / games as f64,
        mean_armed_actions: totals.armed_actions as f64 / games as f64,
        mean_releases: totals.releases as f64 / games as f64,
        mean_release_dodges: totals.release_dodges as f64 / games as f64,
        mean_release_counters: totals.release_counters as f64 / games as f64,
        mean_charge_aborts: totals.charge_aborts as f64 / games as f64,
        mean_charge_overrides: totals.charge_overrides as f64 / games as f64,
        mean_release_overrides: totals.release_overrides as f64 / games as f64,
        mean_attack_expert_actions: totals.attack_expert_actions as f64 / games as f64,
        mean_survival_expert_actions: totals.survival_expert_actions as f64 / games as f64,
        mean_attack_expert_overrides: totals.attack_expert_overrides as f64 / games as f64,
        mean_survival_expert_overrides: totals.survival_expert_overrides as f64 / games as f64,
        mean_hold_fire_actions: totals.hold_fire_actions as f64 / games as f64,
        mean_hold_fire_raw_attack: totals.hold_fire_raw_attack as f64 / games as f64,
        mean_hold_fire_sent: totals.hold_fire_sent as f64 / games as f64,
        mean_charge_release_incoming_edges: totals.charge_release_incoming_edges as f64
            / games as f64,
        mean_charge_release_garbage_rise_edges: totals.charge_release_garbage_rise_edges as f64
            / games as f64,
        mean_charge_release_raw_attack: totals.charge_release_raw_attack as f64 / games as f64,
        mean_charge_release_sent: totals.charge_release_sent as f64 / games as f64,
        mean_ren_starts: totals.ren_starts as f64 / games as f64,
        mean_ren_continuations: totals.ren_continuations as f64 / games as f64,
        mean_stack_ren_entries: totals.stack_ren_entries as f64 / games as f64,
        mean_stack_ren_build_actions: totals.stack_ren_build_actions as f64 / games as f64,
        mean_stack_ren_fires: totals.stack_ren_fires as f64 / games as f64,
        mean_stack_ren_continuations: totals.stack_ren_continuations as f64 / games as f64,
        mean_stack_ren_aborts: totals.stack_ren_aborts as f64 / games as f64,
        mean_stack_ren_fire_sent: totals.stack_ren_fire_sent as f64 / games as f64,
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
    let (hoeffding_low, hoeffding_high) = fixed_sample_hoeffding_interval(mean, values.len());
    let kasane_only = values.iter().filter(|&&value| value > 0.0).count();
    let cold_only = values.iter().filter(|&&value| value < 0.0).count();
    let discordant = kasane_only + cold_only;
    let exact_mcnemar_p_value = exact_one_sided_binomial_tail(kasane_only, discordant);
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
        fixed_sample_hoeffding_ci95_low_pp: hoeffding_low * 100.0,
        fixed_sample_hoeffding_ci95_high_pp: hoeffding_high * 100.0,
        kasane_only_wins: kasane_only,
        cold_clear_only_wins: cold_only,
        target_difference_pp: 15.0,
        target_lower_ci_pp: 10.0,
        passed_ambitious_target: mean >= 0.15 && mean - half_width >= 0.10,
        exploratory_interval_method:
            "normal/Wald 95% interval; retained for compatibility, not used for superiority"
                .to_owned(),
        superiority_method: "fixed-sample two-sided Hoeffding 95% confidence bound for independent paired differences in [-1,1]"
            .to_owned(),
        superior_to_cold_clear: hoeffding_low > 0.0,
        passed_ambitious_target_fixed_sample: mean >= 0.15 && hoeffding_low >= 0.10,
        exact_one_sided_mcnemar_p_value: exact_mcnemar_p_value,
        exact_one_sided_mcnemar_method: "exact one-sided McNemar/binomial test on discordant fixed pairs; H0 p=0.5, H1 p(KASANE-only)>0.5; alpha=0.05; optional stopping is invalid"
            .to_owned(),
        exact_one_sided_mcnemar_rejects_equal_20s_cheese_success_at_alpha_0_05: kasane_only
            > cold_only
            && exact_mcnemar_p_value <= 0.05,
    }
}

fn default_exact_p_value() -> f64 {
    1.0
}

/// P[X >= successes] for X ~ Binomial(trials, 0.5), the exact one-sided
/// McNemar test conditioned on the number of discordant pairs. This is a
/// fixed-sample test; optional stopping or repeated peeking invalidates alpha.
fn exact_one_sided_binomial_tail(successes: usize, trials: usize) -> f64 {
    if trials == 0 || successes == 0 {
        return 1.0;
    }
    debug_assert!(successes <= trials);

    // Calculate the first tail term in log space, then update adjacent terms
    // by their exact ratio. Online log-sum-exp remains stable for large fixed
    // samples where 2^-trials would underflow.
    let mirrored = successes.min(trials - successes);
    let log_choose = (1..=mirrored).fold(0.0, |sum, index| {
        sum + ((trials + 1 - index) as f64).ln() - (index as f64).ln()
    });
    let mut log_probability = log_choose - trials as f64 * std::f64::consts::LN_2;
    let mut log_tail = log_probability;
    for value in successes..trials {
        log_probability += ((trials - value) as f64).ln() - ((value + 1) as f64).ln();
        log_tail = log_add_exp(log_tail, log_probability);
    }
    log_tail.exp().min(1.0)
}

fn log_add_exp(left: f64, right: f64) -> f64 {
    let maximum = left.max(right);
    maximum + ((left.min(right) - maximum).exp()).ln_1p()
}

/// A distribution-free two-sided 1-alpha interval for a fixed number of
/// independent observations with range [-1, 1]. Hoeffding gives
/// P(|mean - E[mean]| >= epsilon) <= 2 exp(-n epsilon^2 / 2).
fn fixed_sample_hoeffding_interval(mean: f64, samples: usize) -> (f64, f64) {
    debug_assert!(samples > 0);
    const ALPHA: f64 = 0.05;
    let epsilon = (2.0 * (2.0 / ALPHA).ln() / samples as f64).sqrt();
    ((mean - epsilon).max(-1.0), (mean + epsilon).min(1.0))
}

/// Small dependency-free SHA-256 implementation used only for benchmark
/// provenance. Keeping it here avoids making the simulator's runtime depend on
/// a hashing crate; the standard test vector below guards the implementation.
#[doc(hidden)]
pub fn sha256_hex(input: &[u8]) -> String {
    const INITIAL: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];

    let bit_len = (input.len() as u64).wrapping_mul(8);
    let mut padded = Vec::with_capacity(input.len() + 72);
    padded.extend_from_slice(input);
    padded.push(0x80);
    while padded.len() % 64 != 56 {
        padded.push(0);
    }
    padded.extend_from_slice(&bit_len.to_be_bytes());

    let mut state = INITIAL;
    for block in padded.chunks_exact(64) {
        let mut words = [0_u32; 64];
        for (index, bytes) in block.chunks_exact(4).enumerate() {
            words[index] = u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
        }
        for index in 16..64 {
            let s0 = words[index - 15].rotate_right(7)
                ^ words[index - 15].rotate_right(18)
                ^ (words[index - 15] >> 3);
            let s1 = words[index - 2].rotate_right(17)
                ^ words[index - 2].rotate_right(19)
                ^ (words[index - 2] >> 10);
            words[index] = words[index - 16]
                .wrapping_add(s0)
                .wrapping_add(words[index - 7])
                .wrapping_add(s1);
        }

        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = state;
        for index in 0..64 {
            let big_s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let choose = (e & f) ^ ((!e) & g);
            let temp1 = h
                .wrapping_add(big_s1)
                .wrapping_add(choose)
                .wrapping_add(K[index])
                .wrapping_add(words[index]);
            let big_s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            let temp2 = big_s0.wrapping_add(majority);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(temp1);
            d = c;
            c = b;
            b = a;
            a = temp1.wrapping_add(temp2);
        }
        state[0] = state[0].wrapping_add(a);
        state[1] = state[1].wrapping_add(b);
        state[2] = state[2].wrapping_add(c);
        state[3] = state[3].wrapping_add(d);
        state[4] = state[4].wrapping_add(e);
        state[5] = state[5].wrapping_add(f);
        state[6] = state[6].wrapping_add(g);
        state[7] = state[7].wrapping_add(h);
    }

    state.iter().map(|word| format!("{word:08x}")).collect()
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

    #[test]
    fn hoeffding_does_not_collapse_on_zero_sample_variance() {
        let (low, high) = fixed_sample_hoeffding_interval(1.0, 4);
        assert!(low < 1.0);
        assert_eq!(high, 1.0);
    }

    #[test]
    fn hoeffding_superiority_requires_enough_fixed_samples() {
        let (seven_low, _) = fixed_sample_hoeffding_interval(1.0, 7);
        let (eight_low, _) = fixed_sample_hoeffding_interval(1.0, 8);
        assert!(seven_low <= 0.0);
        assert!(eight_low > 0.0);
    }

    #[test]
    fn exact_mcnemar_small_sample_threshold_is_not_asymptotic() {
        assert_eq!(exact_one_sided_binomial_tail(0, 0), 1.0);
        assert!((exact_one_sided_binomial_tail(4, 4) - 0.0625).abs() < 1e-12);
        assert!((exact_one_sided_binomial_tail(5, 5) - 0.03125).abs() < 1e-12);
        assert!((exact_one_sided_binomial_tail(3, 5) - 0.5).abs() < 1e-12);
        assert!((exact_one_sided_binomial_tail(9, 10) - 11.0 / 1024.0).abs() < 1e-12);
    }

    #[test]
    fn provenance_hashes_compiled_cold_clear_sources() {
        let provenance = benchmark_provenance(&CheeseBenchmarkConfig::default())
            .expect("default provenance must serialize");
        assert_eq!(provenance.cold_clear_source_artifacts.len(), 2);
        for artifact in provenance.cold_clear_source_artifacts {
            assert_eq!(artifact.sha256.as_deref().map(str::len), Some(64));
            assert!(artifact.read_error.is_none(), "{}", artifact.path);
        }
    }

    #[test]
    fn sha256_matches_standard_vectors() {
        assert_eq!(
            sha256_hex(b""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }
}
