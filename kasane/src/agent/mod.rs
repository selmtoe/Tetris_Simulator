mod cc;
mod guard;
mod kasane;
mod stack_ren;
mod strategy;

use crate::base::BaseModel;
use crate::model::TempoModel;
use crate::rules::Rules;
use crate::search::PlacementAction;
use anyhow::Result;
use libtetris::Board;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

pub use cc::{analyze_cold_clear, forecast_opponent, CcAnalysis, ForecastEvent, OpponentForecast};
pub use stack_ren::{StackRenModel, STACK_REN_FEATURE_NAMES};

#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AgentKind {
    ColdClear,
    Kasane,
    KasaneGuard,
    KasaneStackRen,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Intent {
    Stack,
    SpikeNow,
    CancellationDodge,
    Counter,
    TankThenFire,
    Charge,
    Dig,
    ComboContinue,
    Survival,
}

/// Ground-truth relation to the equal-node Cold Clear floor.  Unlike `Intent`,
/// this is not inferred from the resulting attack: it records whether v2
/// actually changed the placement and/or lock timestamp.
#[derive(Copy, Clone, Debug, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PolicyOverride {
    #[default]
    None,
    SamePlacementDodge,
    SamePlacementCounter,
    SamePlacementWait,
    Placement,
    PlacementAndWait,
}

impl PolicyOverride {
    pub fn changed_placement(self) -> bool {
        matches!(self, Self::Placement | Self::PlacementAndWait)
    }

    pub fn changed_wait(self) -> bool {
        matches!(
            self,
            Self::SamePlacementDodge
                | Self::SamePlacementCounter
                | Self::SamePlacementWait
                | Self::PlacementAndWait
        )
    }
}

/// Persistent-strategy event attached by the stateful v2 controller.  It is
/// orthogonal to `PolicyOverride`: following CC's placement while entering a
/// charge state is a strategy event but not a false placement override.
#[derive(Copy, Clone, Debug, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum StrategyEvent {
    #[default]
    None,
    /// The legacy Base-v3 + strict Tempo policy was selected as the finishing
    /// expert instead of the equal-node Cold Clear survival floor.
    AttackExpert,
    /// Finishing expert kept an already fireable placement and delayed its
    /// lock until the forecast opponent shot (hold-fire cancellation dodge).
    AttackExpertHoldFire,
    /// Equal-node Cold Clear supplied the ordinary/survival action.
    SurvivalExpert,
    /// The survival floor kept a fireable placement and delayed its lock.
    SurvivalExpertHoldFire,
    ChargeEnter,
    ChargeContinue,
    Armed,
    /// Emergency release immediately after a newly queued incoming edge.
    /// Forecast-only release is forbidden.
    ChargeReleaseIncomingEdge,
    /// Offensive release immediately after queued garbage actually rose into
    /// the board, so the stored attack is sent instead of spent cancelling.
    ChargeReleaseGarbageRiseEdge,
    ReleaseDodge,
    ReleaseCounter,
    ReleaseImmediate,
    ChargeAbort,
    /// Entered a queue-proven natural/downstack REN route. Fixed middle
    /// opening construction is intentionally outside v2.
    RenStart,
    /// Preserved an already committed or established REN with a line clear.
    RenContinue,
    /// Entered a persistent two-to-four-column stack well selected by the v3
    /// risk/return model.
    StackRenEnter,
    /// Added a non-clearing placement outside the remembered well.
    StackRenBuild,
    /// Fired the queue-proven REN after the build model crossed its threshold.
    StackRenFire,
    /// Continued the dedicated v3 REN route.
    StackRenContinue,
    /// Abandoned a build because the survival or value gate failed.
    StackRenAbort,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct AgentConfig {
    pub cold_clear_nodes: u32,
    pub kasane_nodes: u32,
    pub forecast_nodes: u32,
    pub maximum_wait_ms: u64,
    pub enable_tank: bool,
    pub dodge_wait_cap_ms: u64,
    pub dodge_finish_height: u32,
    pub dodge_safety_margin: i32,
    pub enable_charge: bool,
    pub strict_base_policy: bool,
    pub base_depth: usize,
    pub base_beam_width: usize,
    pub base_model_path: Option<PathBuf>,
    pub tempo_model_path: Option<PathBuf>,
    pub enable_tempo: bool,
    pub base_guard_margin: f32,
    pub guard_max_headroom_loss: i32,
    pub guard_max_hole_increase: u32,
    pub guard_max_covered_increase: u32,
    pub guard_max_bumpiness_increase: u32,
    pub guard_max_accessible_hole_loss: u32,
    pub guard_minimum_cancel_gain: u32,
    /// Minimum learned advantage required before v2 may replace the equal-node
    /// Cold Clear floor.  This protects against noisy MLP outputs.
    pub strategy_min_advantage: f32,
    /// Extra evidence required for a placement change. Timing-only overrides
    /// do not pay this cost because their geometric result is identical.
    pub strategy_placement_min_advantage: f32,
    /// Number of non-CC placements considered after cheap geometric ranking.
    pub strategy_max_alternatives: usize,
    /// Independent cap for tactical waiting in the v2 policy.
    pub strategy_max_wait_ms: u64,
    pub strategy_enable_stateful: bool,
    pub strategy_charge_min_pieces: u32,
    pub strategy_charge_max_pieces: u32,
    pub strategy_charge_min_headroom: i32,
    pub strategy_release_window_ms: u64,
    pub strategy_counter_offset_ms: u64,
    /// Enable the Cold Clear survival + legacy Basic finishing mixture.
    pub strategy_enable_moe: bool,
    /// Opponent height that permits entering the attack expert when the
    /// opponent forecast is quiet. A higher hard-finish height bypasses the
    /// learned borderline gate so bottom-12 finishing retains Basic exactly.
    pub strategy_attack_enter_height: u32,
    pub strategy_attack_hard_finish_height: u32,
    pub strategy_attack_stay_height: u32,
    pub strategy_attack_hole_burden: u32,
    pub strategy_attack_min_headroom: i32,
    pub strategy_attack_max_due_1000: u32,
    /// Tactical downstack/natural-REN planner. This does not build a fixed
    /// middle opening; it commits only when the visible queue proves a
    /// consecutive-clear route.
    pub strategy_enable_ren: bool,
    pub strategy_ren_depth: usize,
    pub strategy_ren_beam_width: usize,
    pub strategy_ren_start_chain: u32,
    pub strategy_ren_min_headroom: i32,
    /// Dedicated v3 stack-then-REN overlay. These fields are ignored by every
    /// other agent kind, so Strategy v2 remains byte-for-byte selectable.
    pub stack_ren_enable: bool,
    pub stack_ren_model_path: Option<PathBuf>,
    pub stack_ren_min_well_width: usize,
    pub stack_ren_max_well_width: usize,
    pub stack_ren_entry_interval: u32,
    pub stack_ren_action_limit: usize,
    pub stack_ren_min_build_pieces: u32,
    pub stack_ren_max_build_pieces: u32,
    pub stack_ren_max_build_ms: u64,
    pub stack_ren_target_depth: u32,
    pub stack_ren_min_fire_chain: u32,
    pub stack_ren_min_fire_attack: u32,
    pub stack_ren_min_headroom: i32,
    pub stack_ren_max_holes: u32,
    pub stack_ren_max_due_1000: u32,
    pub stack_ren_max_forecast_1000: u32,
    pub stack_ren_entry_threshold: f32,
    pub stack_ren_fire_margin: f32,
    pub stack_ren_abort_margin: f32,
    pub stack_ren_hole_mismatch_penalty: f32,
    pub stack_ren_cooldown_pieces: u32,
    /// In-memory model injection for self-play/evolution. It is intentionally
    /// absent from serialized benchmark configurations.
    #[serde(skip)]
    pub base_model_override: Option<BaseModel>,
    #[serde(skip)]
    pub tempo_model_override: Option<TempoModel>,
    #[serde(skip)]
    pub stack_ren_model_override: Option<StackRenModel>,
}

impl Default for AgentConfig {
    fn default() -> Self {
        Self {
            cold_clear_nodes: 1_500,
            kasane_nodes: 1_500,
            forecast_nodes: 300,
            maximum_wait_ms: 2_000,
            enable_tank: false,
            dodge_wait_cap_ms: 750,
            dodge_finish_height: 13,
            dodge_safety_margin: 5,
            enable_charge: false,
            strict_base_policy: true,
            base_depth: 4,
            base_beam_width: 64,
            base_model_path: None,
            tempo_model_path: None,
            enable_tempo: true,
            base_guard_margin: 6.0,
            guard_max_headroom_loss: 0,
            guard_max_hole_increase: 0,
            guard_max_covered_increase: 0,
            guard_max_bumpiness_increase: 2,
            guard_max_accessible_hole_loss: 0,
            guard_minimum_cancel_gain: 1,
            strategy_min_advantage: 0.35,
            strategy_placement_min_advantage: 3.0,
            strategy_max_alternatives: 16,
            strategy_max_wait_ms: 1_000,
            strategy_enable_stateful: true,
            strategy_charge_min_pieces: 2,
            strategy_charge_max_pieces: 16,
            strategy_charge_min_headroom: 10,
            strategy_release_window_ms: 750,
            strategy_counter_offset_ms: 50,
            strategy_enable_moe: true,
            strategy_attack_enter_height: 8,
            strategy_attack_hard_finish_height: 11,
            strategy_attack_stay_height: 6,
            strategy_attack_hole_burden: 6,
            strategy_attack_min_headroom: 8,
            strategy_attack_max_due_1000: 6,
            strategy_enable_ren: true,
            strategy_ren_depth: 6,
            strategy_ren_beam_width: 20,
            strategy_ren_start_chain: 6,
            strategy_ren_min_headroom: 6,
            stack_ren_enable: false,
            stack_ren_model_path: None,
            stack_ren_min_well_width: 2,
            stack_ren_max_well_width: 4,
            stack_ren_entry_interval: 7,
            stack_ren_action_limit: 48,
            stack_ren_min_build_pieces: 7,
            stack_ren_max_build_pieces: 18,
            stack_ren_max_build_ms: 9_000,
            stack_ren_target_depth: 6,
            stack_ren_min_fire_chain: 6,
            stack_ren_min_fire_attack: 14,
            stack_ren_min_headroom: 10,
            stack_ren_max_holes: 1,
            stack_ren_max_due_1000: 2,
            stack_ren_max_forecast_1000: 3,
            stack_ren_entry_threshold: 8.0,
            stack_ren_fire_margin: 4.0,
            stack_ren_abort_margin: 8.0,
            stack_ren_hole_mismatch_penalty: 24.0,
            stack_ren_cooldown_pieces: 14,
            base_model_override: None,
            tempo_model_override: None,
            stack_ren_model_override: None,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct IncomingPacket {
    pub lines: u32,
    pub arrival_ms: u64,
}

#[derive(Copy, Clone, Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum PhaseView {
    Ready,
    Moving { started_ms: u64 },
    LineClear { ends_ms: u64 },
}

#[derive(Clone, Debug)]
pub struct PlayerView {
    pub board: Board,
    pub can_hold: bool,
    pub incoming: Vec<IncomingPacket>,
    pub phase: PhaseView,
    pub pieces: u64,
    pub average_piece_ms: f32,
}

impl PlayerView {
    pub fn incoming_total(&self) -> u32 {
        self.incoming.iter().map(|packet| packet.lines).sum()
    }
}

#[derive(Clone, Debug)]
pub struct Observation {
    pub now_ms: u64,
    pub rules: Rules,
    pub own: PlayerView,
    pub opponent: PlayerView,
}

#[derive(Clone, Debug)]
pub struct SelectedAction {
    pub action: PlacementAction,
    pub wait_ms: u64,
    pub intent: Intent,
    pub score: f32,
    pub policy_override: PolicyOverride,
    pub strategy_event: StrategyEvent,
    /// Strategy-specific compact metadata. Stack-REN encodes the committed
    /// well as high-nibble width and low-nibble zero-based start column.
    pub strategy_detail: u8,
}

pub struct AgentController {
    kind: AgentKind,
    config: AgentConfig,
    base_model: BaseModel,
    model: TempoModel,
    stack_ren_model: Option<StackRenModel>,
    cold_clear_fallback: Option<Box<AgentController>>,
    strategy_memory: strategy::StrategyMemory,
    stack_ren_memory: stack_ren::StackRenMemory,
    cold_clear_session: Option<cc::ColdClearSession>,
}

impl AgentController {
    pub fn new(kind: AgentKind, config: AgentConfig) -> Result<Self> {
        let base_model = if let Some(model) = &config.base_model_override {
            model.clone()
        } else {
            match &config.base_model_path {
                Some(path) => BaseModel::load(path)?,
                None => BaseModel::default(),
            }
        };
        let model = if let Some(model) = &config.tempo_model_override {
            model.clone()
        } else {
            match &config.tempo_model_path {
                Some(path) => TempoModel::load(path)?,
                None => TempoModel::default(),
            }
        };
        let stack_ren_model = if kind == AgentKind::KasaneStackRen {
            Some(if let Some(model) = &config.stack_ren_model_override {
                model.clone()
            } else {
                match &config.stack_ren_model_path {
                    Some(path) => StackRenModel::load(path)?,
                    None => StackRenModel::default(),
                }
            })
        } else {
            None
        };
        let cold_clear_fallback = if kind == AgentKind::KasaneGuard
            || kind == AgentKind::KasaneStackRen
            || (kind == AgentKind::Kasane && model.is_strategy_v2())
        {
            Some(Box::new(Self::new(AgentKind::ColdClear, config.clone())?))
        } else {
            None
        };
        let cold_clear_session = (kind == AgentKind::ColdClear).then(cc::ColdClearSession::default);
        Ok(Self {
            kind,
            config,
            base_model,
            model,
            stack_ren_model,
            cold_clear_fallback,
            strategy_memory: strategy::StrategyMemory::default(),
            stack_ren_memory: stack_ren::StackRenMemory::default(),
            cold_clear_session,
        })
    }

    pub fn kind(&self) -> AgentKind {
        self.kind
    }

    /// Clears all cross-move strategy and search state while preserving the
    /// controller's model/configuration. Dropping the controller is equivalent,
    /// but an explicit reset keeps embedders from accidentally retaining a DAG.
    pub fn reset(&mut self) {
        self.strategy_memory = strategy::StrategyMemory::default();
        self.stack_ren_memory.reset();
        self.invalidate_search();
    }

    /// Invalidates retained search trees after an external board mutation
    /// while preserving the outer tactical state. Garbage rise uses this so
    /// an armed charge can observe the rise on the next snapshot.
    pub fn invalidate_search(&mut self) {
        if let Some(fallback) = &mut self.cold_clear_fallback {
            fallback.invalidate_search();
        }
        if let Some(session) = &mut self.cold_clear_session {
            session.reset();
        }
    }

    pub fn choose(&mut self, observation: &Observation) -> Option<SelectedAction> {
        let selected = self.choose_proposal(observation);
        self.commit_final_selection(selected.as_ref());
        selected
    }

    /// Produces a candidate without advancing any nested Cold Clear session.
    /// Only the outermost `choose` call knows which placement survived the
    /// strategy/guard override and is therefore allowed to commit the DAG.
    fn choose_proposal(&mut self, observation: &Observation) -> Option<SelectedAction> {
        match self.kind {
            AgentKind::ColdClear => self.choose_cold_clear_proposal(observation),
            AgentKind::KasaneGuard if self.config.enable_tempo => {
                let fallback = self
                    .cold_clear_fallback
                    .as_mut()
                    .and_then(|agent| agent.choose_proposal(observation));
                if self.model.is_strategy_v2() {
                    strategy::choose_strategy_v2(
                        observation,
                        &self.config,
                        &self.base_model,
                        &self.model,
                        fallback,
                        &mut self.strategy_memory,
                    )
                } else {
                    guard::choose_guard(
                        observation,
                        &self.config,
                        &self.base_model,
                        &self.model,
                        fallback,
                    )
                }
            }
            AgentKind::KasaneGuard => self
                .cold_clear_fallback
                .as_mut()
                .and_then(|agent| agent.choose_proposal(observation)),
            AgentKind::KasaneStackRen => {
                let fallback = self
                    .cold_clear_fallback
                    .as_mut()
                    .and_then(|agent| agent.choose_proposal(observation))?;
                if let Some(selected) = stack_ren::choose_stack_ren(
                    observation,
                    &self.config,
                    &self.base_model,
                    self.stack_ren_model
                        .as_ref()
                        .expect("Stack-REN controller owns its dedicated model"),
                    &fallback,
                    &mut self.stack_ren_memory,
                ) {
                    // v2 did not observe the Stack-REN placements. Discard any
                    // stale charge/edge memory before it regains control.
                    self.strategy_memory = strategy::StrategyMemory::default();
                    Some(selected)
                } else {
                    strategy::choose_strategy_v2(
                        observation,
                        &self.config,
                        &self.base_model,
                        &self.model,
                        Some(fallback),
                        &mut self.strategy_memory,
                    )
                }
            }
            AgentKind::Kasane if self.config.enable_tempo && self.model.is_strategy_v2() => {
                let fallback = self
                    .cold_clear_fallback
                    .as_mut()
                    .and_then(|agent| agent.choose_proposal(observation));
                strategy::choose_strategy_v2(
                    observation,
                    &self.config,
                    &self.base_model,
                    &self.model,
                    fallback,
                    &mut self.strategy_memory,
                )
            }
            AgentKind::Kasane if self.config.enable_tempo => {
                kasane::choose_kasane(observation, &self.config, &self.base_model, &self.model)
            }
            AgentKind::Kasane => {
                kasane::choose_base_only(observation, &self.config, &self.base_model)
            }
        }
    }

    fn choose_cold_clear_proposal(&mut self, observation: &Observation) -> Option<SelectedAction> {
        self.cold_clear_session
            .as_mut()
            .and_then(|session| session.choose(observation, &self.config))
    }

    /// Advances the retained floor along the placement that actually reaches
    /// the game. Unsupported override branches intentionally invalidate the
    /// session so the next observation starts from a clean root.
    fn commit_final_selection(&mut self, selected: Option<&SelectedAction>) {
        match self.kind {
            AgentKind::ColdClear => self.commit_cold_clear_selection(selected),
            AgentKind::Kasane | AgentKind::KasaneGuard | AgentKind::KasaneStackRen => {
                if let Some(fallback) = &mut self.cold_clear_fallback {
                    fallback.commit_final_selection(selected);
                }
            }
        }
    }

    fn commit_cold_clear_selection(&mut self, selected: Option<&SelectedAction>) {
        if let Some(session) = &mut self.cold_clear_session {
            session.commit_selected(selected.map(|selected| &selected.action));
        }
    }
}

pub(crate) fn packet_lines_after_cancel(
    packets: &[IncomingPacket],
    mut attack: u32,
) -> Vec<IncomingPacket> {
    let mut remaining = Vec::with_capacity(packets.len());
    for packet in packets {
        let cancelled = attack.min(packet.lines);
        attack -= cancelled;
        if packet.lines > cancelled {
            remaining.push(IncomingPacket {
                lines: packet.lines - cancelled,
                arrival_ms: packet.arrival_ms,
            });
        }
    }
    remaining
}

pub(crate) fn matured_lines(packets: &[IncomingPacket], at_ms: u64, grace_ms: u64) -> u32 {
    packets
        .iter()
        .filter(|packet| at_ms.saturating_sub(packet.arrival_ms) > grace_ms)
        .map(|packet| packet.lines)
        .sum()
}
