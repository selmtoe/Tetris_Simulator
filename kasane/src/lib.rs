//! KASANE: a time-aware two-player Tetris strategy engine.
//!
//! Cold Clear is used as the pinned local-search baseline and as one source of
//! opponent forecasts.  Match timing, garbage packets, tactical actions and
//! the learned tempo scorer are implemented in this crate.

pub mod agent;
pub mod base;
pub mod benchmark;
pub mod evolution;
pub mod game;
pub mod model;
pub mod rules;
pub mod search;
pub mod strategy_training;
pub mod training;

pub use agent::{AgentConfig, AgentKind, Intent, PolicyOverride, StrategyEvent};
pub use game::{Match, MatchConfig, MatchOutcome, MatchResult};
pub use rules::Rules;
