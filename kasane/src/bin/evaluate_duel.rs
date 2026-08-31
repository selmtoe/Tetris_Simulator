use anyhow::{Context, Result};
use clap::Parser;
use kasane::benchmark::sha256_hex;
use kasane::game::{InitialField, PlayerSpec};
use kasane::strategy_training::StrategyPolicyArtifact;
use kasane::{AgentConfig, AgentKind, Match, MatchConfig, MatchOutcome, Rules};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;
use std::time::Instant;

#[derive(Debug, Parser)]
#[command(about = "Run a mirrored KASANE versus Cold Clear league")]
struct Cli {
    /// Number of mirrored seed pairs. The total number of matches is twice this value.
    #[arg(long, default_value_t = 100)]
    pairs: usize,
    #[arg(long, default_value_t = 0x4b41_5341_4e45_9001)]
    seed: u64,
    #[arg(long, default_value_t = 30_000)]
    time_limit_ms: u64,
    #[arg(long, default_value_t = 0)]
    threads: usize,
    #[arg(long, default_value_t = 1_500)]
    cold_clear_nodes: u32,
    /// Node budget for KASANE's embedded Cold Clear safety floor. KASANE's
    /// independent base search is controlled by depth/beam width below.
    /// Omit to use `cold_clear_floor_nodes` from the policy artifact.
    #[arg(long)]
    kasane_nodes: Option<u32>,
    #[arg(long)]
    forecast_nodes: Option<u32>,
    #[arg(long, default_value_t = 4)]
    base_depth: usize,
    #[arg(long, default_value_t = 64)]
    base_beam_width: usize,
    #[arg(long)]
    maximum_wait_ms: Option<u64>,
    #[arg(long, default_value_t = 750)]
    dodge_wait_cap_ms: u64,
    #[arg(long, default_value_t = 13)]
    dodge_finish_height: u32,
    #[arg(long, default_value_t = 5)]
    dodge_safety_margin: i32,
    #[arg(long, default_value_t = 6.0)]
    base_guard_margin: f32,
    #[arg(long)]
    base_model: Option<PathBuf>,
    #[arg(long)]
    tempo_model: Option<PathBuf>,
    /// Full MoE/stateful policy. CLI model/node options override only the
    /// explicitly supplied fields after this artifact is applied.
    #[arg(long, default_value = "config/kasane-strategy-v2.json")]
    strategy_policy: PathBuf,
    /// Evaluate KASANE Guard instead of KASANE Basic.
    #[arg(long)]
    guard: bool,
    /// Evaluate the separate KASANE Stack-REN v3 agent.
    #[arg(long)]
    stack_ren: bool,
    /// Override the Stack-REN neural gate artifact from the policy.
    #[arg(long)]
    stack_ren_model: Option<PathBuf>,
    #[arg(long)]
    stack_ren_min_build_pieces: Option<u32>,
    #[arg(long)]
    stack_ren_max_build_pieces: Option<u32>,
    #[arg(long)]
    stack_ren_target_depth: Option<u32>,
    #[arg(long)]
    stack_ren_min_fire_chain: Option<u32>,
    #[arg(long)]
    stack_ren_min_fire_attack: Option<u32>,
    #[arg(long)]
    stack_ren_entry_threshold: Option<f32>,
    #[arg(long)]
    stack_ren_fire_margin: Option<f32>,
    #[arg(long)]
    stack_ren_abort_margin: Option<f32>,
    #[arg(long)]
    stack_ren_cooldown_pieces: Option<u32>,
    /// Deprecated compatibility switch. Free reranking is the shipped default.
    #[arg(long, hide = true)]
    allow_free_rerank: bool,
    /// Evaluation-only ablation of the browser policy's ordinary-move reranker.
    #[arg(long)]
    strict_base_policy: bool,
    #[arg(long)]
    enable_charge: bool,
    #[arg(long)]
    enable_tank: bool,
    /// Explicit ablation; the policy value is otherwise preserved.
    #[arg(long)]
    disable_stateful_strategy: bool,
    /// Evaluation-only ablation for the tactical REN expert.
    #[arg(long)]
    disable_ren: bool,
    /// Evaluation-only ablation for the attack/survival mixture-of-experts gate.
    #[arg(long)]
    disable_moe: bool,
    #[arg(long)]
    strategy_attack_enter_height: Option<u32>,
    #[arg(long)]
    strategy_attack_hard_finish_height: Option<u32>,
    #[arg(long)]
    strategy_attack_stay_height: Option<u32>,
    #[arg(long)]
    strategy_attack_hole_burden: Option<u32>,
    #[arg(long)]
    strategy_min_advantage: Option<f32>,
    #[arg(long)]
    strategy_placement_min_advantage: Option<f32>,
    #[arg(long)]
    output: Option<PathBuf>,
}

#[derive(Clone, Debug, Default, Serialize)]
struct SideTotals {
    wins: u64,
    losses: u64,
    draws: u64,
    timeouts: u64,
    attack: u64,
    sent: u64,
    cancelled: u64,
    risen: u64,
    pieces: u64,
    waited_ms: u64,
    cancellation_dodges: u64,
    policy_overrides: u64,
    placement_overrides: u64,
    wait_overrides: u64,
    attack_expert_actions: u64,
    survival_expert_actions: u64,
    attack_expert_overrides: u64,
    survival_expert_overrides: u64,
    hold_fire_actions: u64,
    hold_fire_raw_attack: u64,
    hold_fire_sent: u64,
    charge_entries: u64,
    charge_actions: u64,
    armed_actions: u64,
    charge_release_incoming_edges: u64,
    charge_release_garbage_rise_edges: u64,
    charge_release_raw_attack: u64,
    charge_release_sent: u64,
    charge_aborts: u64,
    stack_ren_entries: u64,
    stack_ren_build_actions: u64,
    stack_ren_fires: u64,
    stack_ren_continuations: u64,
    stack_ren_aborts: u64,
    stack_ren_fire_raw_attack: u64,
    stack_ren_fire_sent: u64,
    stack_ren_entry_wells: BTreeMap<String, u64>,
}

#[derive(Clone, Debug, Deserialize)]
struct StackRenPolicyExtras {
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

impl StackRenPolicyExtras {
    fn apply_to(&self, config: &mut AgentConfig) {
        config.stack_ren_model_path = Some(self.stack_ren_model.clone());
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

#[derive(Clone, Debug, Serialize)]
struct PairResult {
    kasane: SideTotals,
    matches: u64,
    ended_ms: u64,
}

#[derive(Debug, Serialize)]
struct Report {
    schema: &'static str,
    rules: Rules,
    config: ReportConfig,
    total_matches: u64,
    kasane: SideTotals,
    decisive_win_rate: f64,
    all_match_score: f64,
    mean_ended_ms: f64,
    decisive_ci95_low: f64,
    decisive_ci95_high: f64,
    /// Exploratory only: seat legs are not independent and timeouts are
    /// conditioned away. Use the paired all-match interval for inference.
    decisive_ci95_method: &'static str,
    /// Mean match score CI with the two seat-swapped games for each seed
    /// treated as one independent cluster. A win scores 1, a loss 0, and a
    /// draw/timeout 0.5.
    paired_score_standard_error: f64,
    paired_score_ci95_low: f64,
    paired_score_ci95_high: f64,
    /// Fixed-sample, distribution-free confidence interval used for the
    /// superiority gate. It is not valid for optional stopping.
    paired_score_ci95_method: &'static str,
    /// One auditable score per seed cluster, in deterministic seed order.
    /// Values are 0, 0.25, 0.5, 0.75, or 1 for the two mirrored legs.
    paired_scores: Vec<f64>,
    /// Conservative, machine-readable superiority gate. This is true only
    /// when the paired all-match score CI excludes an even 50% score.
    paired_score_superior_to_cold_clear: bool,
    provenance: DirectDuelProvenance,
    wall_seconds: f64,
    decisions_per_wall_second: f64,
}

#[derive(Clone, Debug, Serialize)]
struct ArtifactProvenance {
    role: &'static str,
    path: PathBuf,
    sha256: Option<String>,
    read_error: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
struct DirectDuelProvenance {
    comparison_scope: &'static str,
    cold_clear_upstream_revision: &'static str,
    cold_clear_evaluator: &'static str,
    runtime_mode: &'static str,
    deterministic_search_mode: &'static str,
    fixed_sample_design: &'static str,
    artifacts: Vec<ArtifactProvenance>,
}

#[derive(Debug, Serialize)]
struct ReportConfig {
    pairs: usize,
    seed: u64,
    time_limit_ms: u64,
    threads: usize,
    agent: &'static str,
    strategy_policy: PathBuf,
    policy_schema: String,
    cold_clear_nodes: u32,
    kasane_nodes: u32,
    forecast_nodes: u32,
    base_depth: usize,
    base_beam_width: usize,
    base_model: PathBuf,
    tempo_model: PathBuf,
    strict_base_policy: bool,
    enable_charge: bool,
    enable_tank: bool,
    maximum_wait_ms: u64,
    dodge_wait_cap_ms: u64,
    dodge_finish_height: u32,
    dodge_safety_margin: i32,
    base_guard_margin: f32,
    strategy_min_advantage: f32,
    strategy_placement_min_advantage: f32,
    strategy_max_alternatives: usize,
    strategy_max_wait_ms: u64,
    strategy_enable_stateful: bool,
    strategy_charge_min_pieces: u32,
    strategy_charge_max_pieces: u32,
    strategy_charge_min_headroom: i32,
    strategy_release_window_ms: u64,
    strategy_counter_offset_ms: u64,
    strategy_enable_moe: bool,
    strategy_attack_enter_height: u32,
    strategy_attack_hard_finish_height: u32,
    strategy_attack_stay_height: u32,
    strategy_attack_hole_burden: u32,
    strategy_attack_min_headroom: i32,
    strategy_attack_max_due_1000: u32,
    strategy_enable_ren: bool,
    strategy_ren_depth: usize,
    strategy_ren_beam_width: usize,
    strategy_ren_start_chain: u32,
    strategy_ren_min_headroom: i32,
    stack_ren_model: Option<PathBuf>,
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

fn split_seed(seed: u64, index: usize) -> u64 {
    let mut value = seed ^ (index as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15);
    value ^= value >> 30;
    value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value ^= value >> 27;
    value = value.wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

fn artifact_provenance(role: &'static str, path: PathBuf) -> ArtifactProvenance {
    match fs::read(&path) {
        Ok(content) => ArtifactProvenance {
            role,
            path,
            sha256: Some(sha256_hex(&content)),
            read_error: None,
        },
        Err(error) => ArtifactProvenance {
            role,
            path,
            sha256: None,
            read_error: Some(error.to_string()),
        },
    }
}

fn direct_duel_provenance(cli: &Cli, resolved: &AgentConfig) -> DirectDuelProvenance {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let mut artifacts = vec![
        artifact_provenance("strategy_policy", cli.strategy_policy.clone()),
        artifact_provenance(
            "kasane_strategy_source",
            manifest.join("src/agent/strategy.rs"),
        ),
        artifact_provenance("kasane_timing_source", manifest.join("src/agent/kasane.rs")),
        artifact_provenance("kasane_cc_bridge_source", manifest.join("src/agent/cc.rs")),
        artifact_provenance(
            "local_cold_clear_dag_source",
            manifest.join("../third_party/cold-clear-reference/bot/src/dag.rs"),
        ),
        artifact_provenance(
            "local_cold_clear_standard_evaluator",
            manifest.join("../third_party/cold-clear-reference/bot/src/evaluation/standard.rs"),
        ),
    ];
    if let Some(path) = &resolved.base_model_path {
        artifacts.push(artifact_provenance("base_model", path.clone()));
    }
    if let Some(path) = &resolved.tempo_model_path {
        artifacts.push(artifact_provenance("strategy_model", path.clone()));
    }
    if let Some(path) = &resolved.stack_ren_model_path {
        artifacts.push(artifact_provenance("stack_ren_model", path.clone()));
    }
    match std::env::current_exe() {
        Ok(path) => artifacts.push(artifact_provenance("running_executable", path)),
        Err(error) => artifacts.push(ArtifactProvenance {
            role: "running_executable",
            path: PathBuf::new(),
            sha256: None,
            read_error: Some(error.to_string()),
        }),
    }
    DirectDuelProvenance {
        comparison_scope: "native deterministic fixed-node simulator; KASANE includes additional strategy/forecast computation, so this is not an equal-CPU claim",
        cold_clear_upstream_revision: "279edd7c3177ff8077f6a930193397814b281f27",
        cold_clear_evaluator: "Cold Clear 1 Standard evaluator",
        runtime_mode: "persistent generation-aware DAG per player; opponent forecasts are stateless",
        deterministic_search_mode: "stable board/node/incoming seed before every bounded think on native and wasm32 KASANE floor",
        fixed_sample_design: "two seat-swapped legs per precommitted seed; fixed sample only, no optional stopping",
        artifacts,
    }
}

fn agent_config(
    cli: &Cli,
    policy: &StrategyPolicyArtifact,
    stack_policy: Option<&StackRenPolicyExtras>,
) -> AgentConfig {
    let mut config = AgentConfig::default();
    policy.apply_to(&mut config);
    if let Some(stack_policy) = stack_policy {
        stack_policy.apply_to(&mut config);
    }
    // KASANE owns an embedded Cold Clear safety floor. Its budget is the
    // KASANE-side budget; the standalone opponent uses `cold_clear_nodes` in
    // `run_one`. Keeping these separate makes asymmetric calibration runs
    // real instead of merely changing a value printed in the report.
    let kasane_nodes = cli.kasane_nodes.unwrap_or(policy.cold_clear_floor_nodes);
    config.cold_clear_nodes = kasane_nodes;
    config.kasane_nodes = kasane_nodes;
    config.forecast_nodes = cli.forecast_nodes.unwrap_or(policy.forecast_nodes);
    config.base_depth = cli.base_depth;
    config.base_beam_width = cli.base_beam_width;
    config.maximum_wait_ms = cli.maximum_wait_ms.unwrap_or(policy.maximum_wait_ms);
    config.dodge_wait_cap_ms = cli.dodge_wait_cap_ms;
    config.dodge_finish_height = cli.dodge_finish_height;
    config.dodge_safety_margin = cli.dodge_safety_margin;
    config.base_guard_margin = cli.base_guard_margin;
    if let Some(path) = &cli.base_model {
        config.base_model_path = Some(path.clone());
    }
    if let Some(path) = &cli.tempo_model {
        config.tempo_model_path = Some(path.clone());
    }
    if let Some(path) = &cli.stack_ren_model {
        config.stack_ren_model_path = Some(path.clone());
    }
    if let Some(value) = cli.stack_ren_min_build_pieces {
        config.stack_ren_min_build_pieces = value;
    }
    if let Some(value) = cli.stack_ren_max_build_pieces {
        config.stack_ren_max_build_pieces = value;
    }
    if let Some(value) = cli.stack_ren_target_depth {
        config.stack_ren_target_depth = value;
    }
    if let Some(value) = cli.stack_ren_min_fire_chain {
        config.stack_ren_min_fire_chain = value;
    }
    if let Some(value) = cli.stack_ren_min_fire_attack {
        config.stack_ren_min_fire_attack = value;
    }
    if let Some(value) = cli.stack_ren_entry_threshold {
        config.stack_ren_entry_threshold = value;
    }
    if let Some(value) = cli.stack_ren_fire_margin {
        config.stack_ren_fire_margin = value;
    }
    if let Some(value) = cli.stack_ren_abort_margin {
        config.stack_ren_abort_margin = value;
    }
    if let Some(value) = cli.stack_ren_cooldown_pieces {
        config.stack_ren_cooldown_pieces = value;
    }
    // Strategy v2 / Stack-REN v3 ship with free ordinary-move reranking in
    // WASM. Keep native evaluation identical unless strict mode is requested
    // explicitly as an ablation. The old allow flag remains a no-op-compatible
    // way to request the shipped default.
    config.strict_base_policy = cli.strict_base_policy && !cli.allow_free_rerank;
    config.enable_charge = cli.enable_charge;
    config.enable_tank = cli.enable_tank || policy.enable_tank;
    if cli.disable_stateful_strategy {
        config.strategy_enable_stateful = false;
    }
    if cli.disable_ren {
        config.strategy_enable_ren = false;
    }
    if cli.disable_moe {
        config.strategy_enable_moe = false;
    }
    if let Some(value) = cli.strategy_attack_enter_height {
        config.strategy_attack_enter_height = value;
    }
    if let Some(value) = cli.strategy_attack_hard_finish_height {
        config.strategy_attack_hard_finish_height = value;
    }
    if let Some(value) = cli.strategy_attack_stay_height {
        config.strategy_attack_stay_height = value;
    }
    if let Some(value) = cli.strategy_attack_hole_burden {
        config.strategy_attack_hole_burden = value;
    }
    if let Some(value) = cli.strategy_min_advantage {
        config.strategy_min_advantage = value;
    }
    if let Some(value) = cli.strategy_placement_min_advantage {
        config.strategy_placement_min_advantage = value;
    }
    config
}

fn run_one(
    cli: &Cli,
    policy: &StrategyPolicyArtifact,
    stack_policy: Option<&StackRenPolicyExtras>,
    seed: u64,
    kasane_first: bool,
) -> Result<PairResult> {
    let kasane_kind = if cli.stack_ren {
        AgentKind::KasaneStackRen
    } else if cli.guard {
        AgentKind::KasaneGuard
    } else {
        AgentKind::Kasane
    };
    let (players, kasane_index) = if kasane_first {
        (
            [
                PlayerSpec {
                    agent: kasane_kind,
                    initial_field: InitialField::Empty,
                },
                PlayerSpec {
                    agent: AgentKind::ColdClear,
                    initial_field: InitialField::Empty,
                },
            ],
            0,
        )
    } else {
        (
            [
                PlayerSpec {
                    agent: AgentKind::ColdClear,
                    initial_field: InitialField::Empty,
                },
                PlayerSpec {
                    agent: kasane_kind,
                    initial_field: InitialField::Empty,
                },
            ],
            1,
        )
    };
    let kasane_config = agent_config(cli, policy, stack_policy);
    let cc_config = AgentConfig {
        cold_clear_nodes: cli.cold_clear_nodes,
        kasane_nodes: cli.kasane_nodes.unwrap_or(policy.cold_clear_floor_nodes),
        ..AgentConfig::default()
    };
    let agent_configs = if kasane_first {
        [kasane_config, cc_config]
    } else {
        [cc_config, kasane_config]
    };
    let result = Match::new(MatchConfig {
        rules: Rules::live(),
        players,
        agent_configs,
        seed,
        time_limit_ms: cli.time_limit_ms,
        record_trace: false,
    })?
    .run()?;

    let mut totals = SideTotals::default();
    let won = matches!(
        (kasane_index, result.outcome),
        (0, MatchOutcome::Player0Win) | (1, MatchOutcome::Player1Win)
    );
    let lost = matches!(
        (kasane_index, result.outcome),
        (0, MatchOutcome::Player1Win) | (1, MatchOutcome::Player0Win)
    );
    if won {
        totals.wins = 1;
    } else if lost {
        totals.losses = 1;
    } else if result.outcome == MatchOutcome::Draw {
        totals.draws = 1;
    } else {
        totals.timeouts = 1;
    }
    let stats = &result.stats[kasane_index];
    totals.attack = stats.raw_attack;
    totals.sent = stats.sent;
    totals.cancelled = stats.cancelled;
    totals.risen = stats.risen;
    totals.pieces = stats.pieces;
    totals.waited_ms = stats.waited_ms;
    totals.cancellation_dodges = stats.cancellation_dodges;
    totals.policy_overrides = stats.policy_overrides;
    totals.placement_overrides = stats.placement_overrides;
    totals.wait_overrides = stats.wait_overrides;
    totals.attack_expert_actions = stats.attack_expert_actions;
    totals.survival_expert_actions = stats.survival_expert_actions;
    totals.attack_expert_overrides = stats.attack_expert_overrides;
    totals.survival_expert_overrides = stats.survival_expert_overrides;
    totals.hold_fire_actions = stats.hold_fire_actions;
    totals.hold_fire_raw_attack = stats.hold_fire_raw_attack;
    totals.hold_fire_sent = stats.hold_fire_sent;
    totals.charge_entries = stats.charge_entries;
    totals.charge_actions = stats.charge_actions;
    totals.armed_actions = stats.armed_actions;
    totals.charge_release_incoming_edges = stats.charge_release_incoming_edges;
    totals.charge_release_garbage_rise_edges = stats.charge_release_garbage_rise_edges;
    totals.charge_release_raw_attack = stats.charge_release_raw_attack;
    totals.charge_release_sent = stats.charge_release_sent;
    totals.charge_aborts = stats.charge_aborts;
    totals.stack_ren_entries = stats.stack_ren_entries;
    totals.stack_ren_build_actions = stats.stack_ren_build_actions;
    totals.stack_ren_fires = stats.stack_ren_fires;
    totals.stack_ren_continuations = stats.stack_ren_continuations;
    totals.stack_ren_aborts = stats.stack_ren_aborts;
    totals.stack_ren_fire_raw_attack = stats.stack_ren_fire_raw_attack;
    totals.stack_ren_fire_sent = stats.stack_ren_fire_sent;
    totals.stack_ren_entry_wells = stats.stack_ren_entry_wells.clone();
    Ok(PairResult {
        kasane: totals,
        matches: 1,
        ended_ms: result.ended_ms,
    })
}

fn add_totals(target: &mut SideTotals, value: &SideTotals) {
    target.wins += value.wins;
    target.losses += value.losses;
    target.draws += value.draws;
    target.timeouts += value.timeouts;
    target.attack += value.attack;
    target.sent += value.sent;
    target.cancelled += value.cancelled;
    target.risen += value.risen;
    target.pieces += value.pieces;
    target.waited_ms += value.waited_ms;
    target.cancellation_dodges += value.cancellation_dodges;
    target.policy_overrides += value.policy_overrides;
    target.placement_overrides += value.placement_overrides;
    target.wait_overrides += value.wait_overrides;
    target.attack_expert_actions += value.attack_expert_actions;
    target.survival_expert_actions += value.survival_expert_actions;
    target.attack_expert_overrides += value.attack_expert_overrides;
    target.survival_expert_overrides += value.survival_expert_overrides;
    target.hold_fire_actions += value.hold_fire_actions;
    target.hold_fire_raw_attack += value.hold_fire_raw_attack;
    target.hold_fire_sent += value.hold_fire_sent;
    target.charge_entries += value.charge_entries;
    target.charge_actions += value.charge_actions;
    target.armed_actions += value.armed_actions;
    target.charge_release_incoming_edges += value.charge_release_incoming_edges;
    target.charge_release_garbage_rise_edges += value.charge_release_garbage_rise_edges;
    target.charge_release_raw_attack += value.charge_release_raw_attack;
    target.charge_release_sent += value.charge_release_sent;
    target.charge_aborts += value.charge_aborts;
    target.stack_ren_entries += value.stack_ren_entries;
    target.stack_ren_build_actions += value.stack_ren_build_actions;
    target.stack_ren_fires += value.stack_ren_fires;
    target.stack_ren_continuations += value.stack_ren_continuations;
    target.stack_ren_aborts += value.stack_ren_aborts;
    target.stack_ren_fire_raw_attack += value.stack_ren_fire_raw_attack;
    target.stack_ren_fire_sent += value.stack_ren_fire_sent;
    for (well, count) in &value.stack_ren_entry_wells {
        *target
            .stack_ren_entry_wells
            .entry(well.clone())
            .or_default() += count;
    }
}

fn wilson(successes: u64, total: u64) -> (f64, f64) {
    if total == 0 {
        return (0.0, 1.0);
    }
    let n = total as f64;
    let p = successes as f64 / n;
    let z = 1.959_963_984_540_054;
    let denominator = 1.0 + z * z / n;
    let center = (p + z * z / (2.0 * n)) / denominator;
    let margin = z * ((p * (1.0 - p) / n + z * z / (4.0 * n * n)).sqrt()) / denominator;
    ((center - margin).max(0.0), (center + margin).min(1.0))
}

fn match_score(totals: &SideTotals) -> f64 {
    if totals.wins > 0 {
        1.0
    } else if totals.losses > 0 {
        0.0
    } else {
        0.5
    }
}

fn mirrored_pair_scores(results: &[PairResult]) -> Vec<f64> {
    results
        .chunks_exact(2)
        .map(|pair| (match_score(&pair[0].kasane) + match_score(&pair[1].kasane)) / 2.0)
        .collect()
}

/// Two-sided Hoeffding interval over mirrored seed pairs. The two matches in
/// one pair share the piece/garbage seed and are therefore counted as one
/// independent bounded observation. Unlike a Wald interval, this remains
/// conservative when every observed pair has the same score.
fn paired_score_interval(results: &[PairResult]) -> (f64, f64, f64) {
    let pair_scores = mirrored_pair_scores(results);
    if pair_scores.is_empty() {
        return (0.0, 0.0, 1.0);
    }
    let n = pair_scores.len() as f64;
    let mean = pair_scores.iter().sum::<f64>() / n;
    let standard_error = if pair_scores.len() == 1 {
        0.0
    } else {
        let sample_variance = pair_scores
            .iter()
            .map(|score| (score - mean).powi(2))
            .sum::<f64>()
            / (n - 1.0);
        (sample_variance / n).sqrt()
    };
    // P(|sample mean - population mean| >= epsilon) <= 2e^(-2n epsilon^2).
    let margin = ((2.0_f64 / 0.05).ln() / (2.0 * n)).sqrt();
    (
        standard_error,
        (mean - margin).max(0.0),
        (mean + margin).min(1.0),
    )
}

fn run(cli: Cli) -> Result<()> {
    if cli.pairs == 0 {
        anyhow::bail!("pairs must be positive");
    }
    if cli.guard && cli.stack_ren {
        anyhow::bail!("--guard and --stack-ren are mutually exclusive");
    }
    let policy_text = fs::read_to_string(&cli.strategy_policy).with_context(|| {
        format!(
            "failed to read strategy policy {}",
            cli.strategy_policy.display()
        )
    })?;
    let policy: StrategyPolicyArtifact = serde_json::from_str(&policy_text).with_context(|| {
        format!(
            "failed to parse strategy policy {}",
            cli.strategy_policy.display()
        )
    })?;
    let stack_policy = if cli.stack_ren {
        Some(
            serde_json::from_str::<StackRenPolicyExtras>(&policy_text).with_context(|| {
                format!(
                    "failed to parse Stack-REN fields from {}",
                    cli.strategy_policy.display()
                )
            })?,
        )
    } else {
        None
    };
    let resolved = agent_config(&cli, &policy, stack_policy.as_ref());
    // Snapshot every runtime artifact before workers begin. Long fixed-sample
    // leagues must not hash a file that changed after its model was loaded.
    let provenance = direct_duel_provenance(&cli, &resolved);
    let jobs: Vec<_> = (0..cli.pairs)
        .flat_map(|index| {
            let seed = split_seed(cli.seed, index);
            [(seed, true), (seed, false)]
        })
        .collect();
    let execute = || -> Result<Vec<PairResult>> {
        // Collect the IndexedParallelIterator before transposing Results.
        // Rayon then preserves job order, which is required by the adjacent
        // mirrored-leg clustering below.
        let indexed_results: Vec<Result<PairResult>> = jobs
            .par_iter()
            .map(|&(seed, kasane_first)| {
                run_one(&cli, &policy, stack_policy.as_ref(), seed, kasane_first)
            })
            .collect();
        indexed_results.into_iter().collect()
    };
    let started = Instant::now();
    let results = if cli.threads == 0 {
        execute()?
    } else {
        rayon::ThreadPoolBuilder::new()
            .num_threads(cli.threads)
            .build()?
            .install(execute)?
    };
    let mut totals = SideTotals::default();
    let mut total_matches = 0_u64;
    let mut ended_ms = 0_u64;
    for result in &results {
        add_totals(&mut totals, &result.kasane);
        total_matches += result.matches;
        ended_ms += result.ended_ms;
    }
    let decisive = totals.wins + totals.losses;
    let (decisive_ci95_low, decisive_ci95_high) = wilson(totals.wins, decisive);
    let (paired_score_standard_error, paired_score_ci95_low, paired_score_ci95_high) =
        paired_score_interval(&results);
    let paired_scores = mirrored_pair_scores(&results);
    let wall_seconds = started.elapsed().as_secs_f64();
    let report = Report {
        schema: if cli.stack_ren {
            "kasane-direct-duel/v5-stack-ren-fixed-sample-provenance"
        } else {
            "kasane-direct-duel/v5-moe-fixed-sample-provenance"
        },
        rules: Rules::live(),
        config: ReportConfig {
            pairs: cli.pairs,
            seed: cli.seed,
            time_limit_ms: cli.time_limit_ms,
            threads: cli.threads,
            agent: if cli.stack_ren {
                "kasane_stack_ren"
            } else if cli.guard {
                "kasane_guard"
            } else {
                "kasane"
            },
            strategy_policy: cli.strategy_policy.clone(),
            policy_schema: policy.schema.clone(),
            cold_clear_nodes: cli.cold_clear_nodes,
            kasane_nodes: resolved.kasane_nodes,
            forecast_nodes: resolved.forecast_nodes,
            base_depth: cli.base_depth,
            base_beam_width: cli.base_beam_width,
            base_model: resolved
                .base_model_path
                .clone()
                .expect("policy supplies base model"),
            tempo_model: resolved
                .tempo_model_path
                .clone()
                .expect("policy supplies strategy model"),
            strict_base_policy: resolved.strict_base_policy,
            enable_charge: cli.enable_charge,
            enable_tank: resolved.enable_tank,
            maximum_wait_ms: resolved.maximum_wait_ms,
            dodge_wait_cap_ms: cli.dodge_wait_cap_ms,
            dodge_finish_height: cli.dodge_finish_height,
            dodge_safety_margin: cli.dodge_safety_margin,
            base_guard_margin: cli.base_guard_margin,
            strategy_min_advantage: resolved.strategy_min_advantage,
            strategy_placement_min_advantage: resolved.strategy_placement_min_advantage,
            strategy_max_alternatives: resolved.strategy_max_alternatives,
            strategy_max_wait_ms: resolved.strategy_max_wait_ms,
            strategy_enable_stateful: resolved.strategy_enable_stateful,
            strategy_charge_min_pieces: resolved.strategy_charge_min_pieces,
            strategy_charge_max_pieces: resolved.strategy_charge_max_pieces,
            strategy_charge_min_headroom: resolved.strategy_charge_min_headroom,
            strategy_release_window_ms: resolved.strategy_release_window_ms,
            strategy_counter_offset_ms: resolved.strategy_counter_offset_ms,
            strategy_enable_moe: resolved.strategy_enable_moe,
            strategy_attack_enter_height: resolved.strategy_attack_enter_height,
            strategy_attack_hard_finish_height: resolved.strategy_attack_hard_finish_height,
            strategy_attack_stay_height: resolved.strategy_attack_stay_height,
            strategy_attack_hole_burden: resolved.strategy_attack_hole_burden,
            strategy_attack_min_headroom: resolved.strategy_attack_min_headroom,
            strategy_attack_max_due_1000: resolved.strategy_attack_max_due_1000,
            strategy_enable_ren: resolved.strategy_enable_ren,
            strategy_ren_depth: resolved.strategy_ren_depth,
            strategy_ren_beam_width: resolved.strategy_ren_beam_width,
            strategy_ren_start_chain: resolved.strategy_ren_start_chain,
            strategy_ren_min_headroom: resolved.strategy_ren_min_headroom,
            stack_ren_model: resolved.stack_ren_model_path.clone(),
            stack_ren_enable: resolved.stack_ren_enable,
            stack_ren_min_well_width: resolved.stack_ren_min_well_width,
            stack_ren_max_well_width: resolved.stack_ren_max_well_width,
            stack_ren_entry_interval: resolved.stack_ren_entry_interval,
            stack_ren_action_limit: resolved.stack_ren_action_limit,
            stack_ren_min_build_pieces: resolved.stack_ren_min_build_pieces,
            stack_ren_max_build_pieces: resolved.stack_ren_max_build_pieces,
            stack_ren_max_build_ms: resolved.stack_ren_max_build_ms,
            stack_ren_target_depth: resolved.stack_ren_target_depth,
            stack_ren_min_fire_chain: resolved.stack_ren_min_fire_chain,
            stack_ren_min_fire_attack: resolved.stack_ren_min_fire_attack,
            stack_ren_min_headroom: resolved.stack_ren_min_headroom,
            stack_ren_max_holes: resolved.stack_ren_max_holes,
            stack_ren_max_due_1000: resolved.stack_ren_max_due_1000,
            stack_ren_max_forecast_1000: resolved.stack_ren_max_forecast_1000,
            stack_ren_entry_threshold: resolved.stack_ren_entry_threshold,
            stack_ren_fire_margin: resolved.stack_ren_fire_margin,
            stack_ren_abort_margin: resolved.stack_ren_abort_margin,
            stack_ren_hole_mismatch_penalty: resolved.stack_ren_hole_mismatch_penalty,
            stack_ren_cooldown_pieces: resolved.stack_ren_cooldown_pieces,
        },
        total_matches,
        decisive_win_rate: if decisive == 0 {
            0.5
        } else {
            totals.wins as f64 / decisive as f64
        },
        all_match_score: (totals.wins as f64 + 0.5 * (totals.draws + totals.timeouts) as f64)
            / total_matches as f64,
        mean_ended_ms: ended_ms as f64 / total_matches as f64,
        decisive_ci95_low,
        decisive_ci95_high,
        decisive_ci95_method:
            "exploratory unclustered Wilson interval over decisive legs; excludes timeouts",
        paired_score_standard_error,
        paired_score_ci95_low,
        paired_score_ci95_high,
        paired_score_ci95_method: "two-sided Hoeffding bound over mirrored seed clusters",
        paired_scores,
        paired_score_superior_to_cold_clear: paired_score_ci95_low > 0.5,
        provenance,
        wall_seconds,
        decisions_per_wall_second: totals.pieces as f64 / wall_seconds.max(1e-9),
        kasane: totals,
    };
    let json = serde_json::to_string_pretty(&report)?;
    if let Some(path) = &cli.output {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)
                .with_context(|| format!("failed to create {}", parent.display()))?;
        }
        fs::write(path, &json).with_context(|| format!("failed to write {}", path.display()))?;
    }
    println!("{json}");
    Ok(())
}

fn main() -> Result<()> {
    run(Cli::parse())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn result(wins: u64, losses: u64) -> PairResult {
        PairResult {
            kasane: SideTotals {
                wins,
                losses,
                ..SideTotals::default()
            },
            matches: 1,
            ended_ms: 0,
        }
    }

    #[test]
    fn paired_interval_clusters_the_two_seats() {
        let results = vec![result(1, 0), result(1, 0), result(1, 0), result(0, 1)];
        let (standard_error, low, high) = paired_score_interval(&results);
        assert!((standard_error - 0.25).abs() < 1e-12);
        assert_eq!(low, 0.0);
        assert_eq!(high, 1.0);
    }

    #[test]
    fn paired_interval_keeps_timeouts_as_neutral_scores() {
        let results = vec![result(0, 0), result(0, 0), result(0, 0), result(0, 0)];
        let (standard_error, low, high) = paired_score_interval(&results);
        assert_eq!(standard_error, 0.0);
        assert_eq!(low, 0.0);
        assert_eq!(high, 1.0);
    }

    #[test]
    fn paired_interval_does_not_claim_certainty_from_identical_small_sample() {
        let results = vec![result(1, 0), result(0, 0), result(1, 0), result(0, 0)];
        let (_, low, high) = paired_score_interval(&results);
        assert_eq!(low, 0.0);
        assert_eq!(high, 1.0);
    }
}
