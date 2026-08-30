mod cc;
mod guard;
mod kasane;

use crate::base::BaseModel;
use crate::model::TempoModel;
use crate::rules::Rules;
use crate::search::PlacementAction;
use anyhow::Result;
use libtetris::Board;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

pub use cc::{analyze_cold_clear, forecast_opponent, CcAnalysis, ForecastEvent, OpponentForecast};

#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AgentKind {
    ColdClear,
    Kasane,
    KasaneGuard,
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
    /// In-memory model injection for self-play/evolution. It is intentionally
    /// absent from serialized benchmark configurations.
    #[serde(skip)]
    pub base_model_override: Option<BaseModel>,
    #[serde(skip)]
    pub tempo_model_override: Option<TempoModel>,
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
            base_model_override: None,
            tempo_model_override: None,
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
}

pub struct AgentController {
    kind: AgentKind,
    config: AgentConfig,
    base_model: BaseModel,
    model: TempoModel,
    cold_clear_fallback: Option<Box<AgentController>>,
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
        let cold_clear_fallback = if kind == AgentKind::KasaneGuard {
            Some(Box::new(Self::new(AgentKind::ColdClear, config.clone())?))
        } else {
            None
        };
        Ok(Self {
            kind,
            config,
            base_model,
            model,
            cold_clear_fallback,
        })
    }

    pub fn kind(&self) -> AgentKind {
        self.kind
    }

    pub fn choose(&mut self, observation: &Observation) -> Option<SelectedAction> {
        match self.kind {
            AgentKind::ColdClear => cc::choose_cold_clear(observation, &self.config),
            AgentKind::KasaneGuard if self.config.enable_tempo => {
                let fallback = self
                    .cold_clear_fallback
                    .as_mut()
                    .and_then(|agent| agent.choose(observation));
                guard::choose_guard(
                    observation,
                    &self.config,
                    &self.base_model,
                    &self.model,
                    fallback,
                )
            }
            AgentKind::KasaneGuard => self
                .cold_clear_fallback
                .as_mut()
                .and_then(|agent| agent.choose(observation)),
            AgentKind::Kasane if self.config.enable_tempo => {
                kasane::choose_kasane(observation, &self.config, &self.base_model, &self.model)
            }
            AgentKind::Kasane => {
                kasane::choose_base_only(observation, &self.config, &self.base_model)
            }
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
