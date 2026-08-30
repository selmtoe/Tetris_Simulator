use crate::agent::{
    AgentConfig, AgentController, AgentKind, IncomingPacket, Intent, Observation, PhaseView,
    PlayerView, SelectedAction,
};
use crate::rules::Rules;
use crate::search::attack_with_pc;
use anyhow::{bail, Result};
use libtetris::Board;
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, VecDeque};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum InitialField {
    Empty,
    Cheese { rows: u32, strict_hole_bara: bool },
}

impl Default for InitialField {
    fn default() -> Self {
        Self::Empty
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PlayerSpec {
    pub agent: AgentKind,
    #[serde(default)]
    pub initial_field: InitialField,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct MatchConfig {
    pub rules: Rules,
    pub players: [PlayerSpec; 2],
    pub agent_configs: [AgentConfig; 2],
    pub seed: u64,
    pub time_limit_ms: u64,
    pub record_trace: bool,
}

impl Default for MatchConfig {
    fn default() -> Self {
        Self {
            rules: Rules::pinned(),
            players: [
                PlayerSpec {
                    agent: AgentKind::Kasane,
                    initial_field: InitialField::Empty,
                },
                PlayerSpec {
                    agent: AgentKind::ColdClear,
                    initial_field: InitialField::Empty,
                },
            ],
            agent_configs: [AgentConfig::default(), AgentConfig::default()],
            seed: 1,
            time_limit_ms: 30_000,
            record_trace: false,
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MatchOutcome {
    Player0Win,
    Player1Win,
    Draw,
    Timeout,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct PlayerStats {
    pub pieces: u64,
    pub lines: u64,
    pub raw_attack: u64,
    pub cancelled: u64,
    pub sent: u64,
    pub received: u64,
    pub risen: u64,
    pub perfect_clears: u64,
    pub max_combo: u32,
    pub waited_ms: u64,
    pub cancellation_dodges: u64,
    pub tank_actions: u64,
    pub intents: BTreeMap<String, u64>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct EventRecord {
    pub at_ms: u64,
    pub player: usize,
    pub kind: String,
    pub lines: u32,
    pub intent: Option<Intent>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct MatchResult {
    pub outcome: MatchOutcome,
    pub ended_ms: u64,
    pub dead: [bool; 2],
    pub stats: [PlayerStats; 2],
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub trace: Vec<EventRecord>,
}

pub struct Match {
    config: MatchConfig,
}

impl Match {
    pub fn new(config: MatchConfig) -> Result<Self> {
        config.rules.validate()?;
        if config.time_limit_ms == 0 {
            bail!("time_limit_ms must be positive");
        }
        for player in &config.players {
            if let InitialField::Cheese { rows, .. } = player.initial_field {
                if rows > 39 {
                    bail!("initial cheese cannot exceed 39 rows");
                }
            }
        }
        Ok(Self { config })
    }

    pub fn run(self) -> Result<MatchResult> {
        Engine::new(self.config)?.run()
    }
}

#[derive(Clone, Debug)]
struct GarbagePacket {
    lines: u32,
    arrival_ms: u64,
}

enum Phase {
    Ready,
    Moving {
        started_ms: u64,
        lock_ms: u64,
        selected: SelectedAction,
    },
    LineClear {
        ends_ms: u64,
    },
}

struct PlayerRuntime {
    board: Board,
    incoming: VecDeque<GarbagePacket>,
    phase: Phase,
    agent: AgentController,
    piece_rng: StdRng,
    garbage_rng: StdRng,
    last_garbage_hole: Option<usize>,
    dead: bool,
    stats: PlayerStats,
}

impl PlayerRuntime {
    fn view(&self, now_ms: u64) -> PlayerView {
        let phase = match self.phase {
            Phase::Ready => PhaseView::Ready,
            Phase::Moving { started_ms, .. } => PhaseView::Moving { started_ms },
            Phase::LineClear { ends_ms } => PhaseView::LineClear { ends_ms },
        };
        PlayerView {
            board: self.board.clone(),
            incoming: self
                .incoming
                .iter()
                .map(|packet| IncomingPacket {
                    lines: packet.lines,
                    arrival_ms: packet.arrival_ms,
                })
                .collect(),
            phase,
            pieces: self.stats.pieces,
            average_piece_ms: if self.stats.pieces == 0 {
                350.0
            } else {
                now_ms as f32 / self.stats.pieces as f32
            },
        }
    }

    fn cancel_incoming(&mut self, mut attack: u32) -> u32 {
        let original = attack;
        while attack > 0 {
            let Some(front) = self.incoming.front_mut() else {
                break;
            };
            let cancelled = attack.min(front.lines);
            attack -= cancelled;
            front.lines -= cancelled;
            if front.lines == 0 {
                self.incoming.pop_front();
            }
        }
        original - attack
    }

    fn replenish_queue(&mut self, count: usize) {
        for _ in 0..count {
            let piece = self.board.generate_next_piece(&mut self.piece_rng);
            self.board.add_next_piece(piece);
        }
    }

    fn rise_matured(&mut self, now_ms: u64, rules: &Rules) -> u32 {
        let mut lines = 0;
        let mut retained = VecDeque::with_capacity(self.incoming.len());
        while let Some(packet) = self.incoming.pop_front() {
            if now_ms.saturating_sub(packet.arrival_ms) > rules.garbage_grace_ms {
                lines += packet.lines;
            } else {
                retained.push_back(packet);
            }
        }
        self.incoming = retained;
        for _ in 0..lines {
            let mut hole = self
                .last_garbage_hole
                .unwrap_or_else(|| self.garbage_rng.gen_range(0, 10));
            if self.garbage_rng.gen::<f64>() < rules.garbage_randomness {
                hole = self.garbage_rng.gen_range(0, 10);
            }
            self.last_garbage_hole = Some(hole);
            if self.board.add_garbage(hole) {
                self.dead = true;
                break;
            }
            self.stats.risen += 1;
        }
        lines
    }
}

struct Engine {
    config: MatchConfig,
    players: [PlayerRuntime; 2],
    now_ms: u64,
    trace: Vec<EventRecord>,
}

impl Engine {
    fn new(config: MatchConfig) -> Result<Self> {
        let player0 = make_player(
            &config.players[0],
            config.agent_configs[0].clone(),
            split_seed(config.seed, 0),
            config.rules.preview_count,
        )?;
        let player1 = make_player(
            &config.players[1],
            config.agent_configs[1].clone(),
            split_seed(config.seed, 1),
            config.rules.preview_count,
        )?;
        Ok(Self {
            config,
            players: [player0, player1],
            now_ms: 0,
            trace: Vec::new(),
        })
    }

    fn run(mut self) -> Result<MatchResult> {
        self.schedule_ready();
        while !self.players.iter().any(|player| player.dead) {
            let Some(next_ms) = self.next_event_ms() else {
                self.players[0].dead = true;
                self.players[1].dead = true;
                break;
            };
            if next_ms > self.config.time_limit_ms {
                self.now_ms = self.config.time_limit_ms;
                break;
            }
            self.now_ms = next_ms;
            self.finish_line_delays();
            self.process_locks();
            if self.players.iter().any(|player| player.dead) {
                break;
            }
            self.schedule_ready();
        }
        let dead = [self.players[0].dead, self.players[1].dead];
        let outcome = match dead {
            [false, true] => MatchOutcome::Player0Win,
            [true, false] => MatchOutcome::Player1Win,
            [true, true] => MatchOutcome::Draw,
            [false, false] => MatchOutcome::Timeout,
        };
        let [left, right] = self.players;
        Ok(MatchResult {
            outcome,
            ended_ms: self.now_ms,
            dead,
            stats: [left.stats, right.stats],
            trace: self.trace,
        })
    }

    fn next_event_ms(&self) -> Option<u64> {
        self.players
            .iter()
            .filter_map(|player| match &player.phase {
                Phase::Ready => None,
                Phase::Moving { lock_ms, .. } => Some(*lock_ms),
                Phase::LineClear { ends_ms } => Some(*ends_ms),
            })
            .min()
    }

    fn finish_line_delays(&mut self) {
        for index in 0..2 {
            let ends_now = matches!(
                self.players[index].phase,
                Phase::LineClear { ends_ms } if ends_ms == self.now_ms
            );
            if !ends_now {
                continue;
            }
            let risen = self.players[index].rise_matured(self.now_ms, &self.config.rules);
            self.record(index, "garbage_rise", risen, None);
            if !self.players[index].dead {
                self.players[index].phase = Phase::Ready;
            }
        }
    }

    fn process_locks(&mut self) {
        let mut locking: [Option<SelectedAction>; 2] = [None, None];
        for index in 0..2 {
            let should_lock = matches!(
                self.players[index].phase,
                Phase::Moving { lock_ms, .. } if lock_ms == self.now_ms
            );
            if should_lock {
                let old = std::mem::replace(&mut self.players[index].phase, Phase::Ready);
                if let Phase::Moving { selected, .. } = old {
                    locking[index] = Some(selected);
                }
            }
        }
        if locking.iter().all(Option::is_none) {
            return;
        }

        let mut outgoing = [0_u32; 2];
        for index in 0..2 {
            let Some(selected) = locking[index].take() else {
                continue;
            };
            let raw_attack = attack_with_pc(
                &selected.action.lock,
                self.config.rules.perfect_clear_special_attack,
            );
            let line_count = selected.action.lock.cleared_lines.len() as u32;
            let player = &mut self.players[index];
            player.board = selected.action.board_after.clone();
            player.replenish_queue(selected.action.pieces_consumed);
            player.stats.pieces += 1;
            player.stats.lines += line_count as u64;
            player.stats.raw_attack += raw_attack as u64;
            player.stats.waited_ms += selected.wait_ms;
            player.stats.perfect_clears += selected.action.lock.perfect_clear as u64;
            player.stats.max_combo = player
                .stats
                .max_combo
                .max(selected.action.lock.combo.unwrap_or(0));
            *player
                .stats
                .intents
                .entry(intent_name(selected.intent).to_owned())
                .or_default() += 1;
            if selected.intent == Intent::CancellationDodge {
                player.stats.cancellation_dodges += 1;
            }
            if selected.intent == Intent::TankThenFire {
                player.stats.tank_actions += 1;
            }
            let cancelled = player.cancel_incoming(raw_attack);
            outgoing[index] = raw_attack - cancelled;
            player.stats.cancelled += cancelled as u64;
            player.stats.sent += outgoing[index] as u64;
            if selected.action.lock.locked_out {
                player.dead = true;
            }
            if line_count > 0 {
                player.phase = Phase::LineClear {
                    ends_ms: self.now_ms + self.config.rules.line_clear_delay_ms,
                };
            } else {
                let risen = player.rise_matured(self.now_ms, &self.config.rules);
                if self.config.record_trace && risen > 0 {
                    self.trace.push(EventRecord {
                        at_ms: self.now_ms,
                        player: index,
                        kind: "garbage_rise".to_owned(),
                        lines: risen,
                        intent: None,
                    });
                }
                if !player.dead {
                    player.phase = Phase::Ready;
                }
            }
            self.record(index, "lock", raw_attack, Some(selected.intent));
        }

        // Attacks produced at the same timestamp cannot cancel each other:
        // both players first cancel their old queues, then leftovers arrive.
        for source in 0..2 {
            let lines = outgoing[source];
            if lines == 0 {
                continue;
            }
            let target = 1 - source;
            self.players[target].incoming.push_back(GarbagePacket {
                lines,
                arrival_ms: self.now_ms,
            });
            self.players[target].stats.received += lines as u64;
            self.record(source, "send", lines, None);
        }
    }

    fn schedule_ready(&mut self) {
        let ready = [0, 1].map(|index| {
            !self.players[index].dead && matches!(self.players[index].phase, Phase::Ready)
        });
        if !ready[0] && !ready[1] {
            return;
        }
        // Freeze both observations before either agent is invoked. This keeps
        // simultaneous decisions symmetric and hides selected placements.
        let views = [
            self.players[0].view(self.now_ms),
            self.players[1].view(self.now_ms),
        ];
        for index in 0..2 {
            if !ready[index] {
                continue;
            }
            let observation = Observation {
                now_ms: self.now_ms,
                rules: self.config.rules.clone(),
                own: views[index].clone(),
                opponent: views[1 - index].clone(),
            };
            let selected = self.players[index].agent.choose(&observation);
            let Some(selected) = selected else {
                self.players[index].dead = true;
                continue;
            };
            let controller_ms = self
                .config
                .rules
                .controller_time_ms(selected.action.movements.len(), selected.action.hold);
            let lock_ms = self.now_ms
                + self.config.rules.decision_latency_ms
                + selected.wait_ms
                + controller_ms;
            self.players[index].phase = Phase::Moving {
                started_ms: self.now_ms,
                lock_ms,
                selected,
            };
        }
    }

    fn record(&mut self, player: usize, kind: &str, lines: u32, intent: Option<Intent>) {
        if self.config.record_trace && (lines > 0 || kind == "lock") {
            self.trace.push(EventRecord {
                at_ms: self.now_ms,
                player,
                kind: kind.to_owned(),
                lines,
                intent,
            });
        }
    }
}

fn make_player(
    spec: &PlayerSpec,
    config: AgentConfig,
    seed: u64,
    preview_count: usize,
) -> Result<PlayerRuntime> {
    let mut field_rng = StdRng::seed_from_u64(split_seed(seed, 10));
    let mut piece_rng = StdRng::seed_from_u64(split_seed(seed, 20));
    let garbage_rng = StdRng::seed_from_u64(split_seed(seed, 30));
    let mut board = Board::new();
    if let InitialField::Cheese {
        rows,
        strict_hole_bara,
    } = spec.initial_field
    {
        let field = cheese_field(rows, strict_hole_bara, &mut field_rng);
        board.set_field(field);
    }
    for _ in 0..=preview_count {
        let piece = board.generate_next_piece(&mut piece_rng);
        board.add_next_piece(piece);
    }
    Ok(PlayerRuntime {
        board,
        incoming: VecDeque::new(),
        phase: Phase::Ready,
        agent: AgentController::new(spec.agent, config)?,
        piece_rng,
        garbage_rng,
        last_garbage_hole: None,
        dead: false,
        stats: PlayerStats::default(),
    })
}

fn cheese_field(rows: u32, strict: bool, rng: &mut StdRng) -> [[bool; 10]; 40] {
    let mut field = [[false; 10]; 40];
    let mut previous_hole = None;
    for y in 0..rows.min(40) as usize {
        let hole = if strict {
            if let Some(previous) = previous_hole {
                let choice = rng.gen_range(0, 9);
                if choice >= previous {
                    choice + 1
                } else {
                    choice
                }
            } else {
                rng.gen_range(0, 10)
            }
        } else {
            rng.gen_range(0, 10)
        };
        previous_hole = Some(hole);
        for x in 0..10 {
            field[y][x] = x != hole;
        }
    }
    field
}

fn split_seed(seed: u64, stream: u64) -> u64 {
    let mut value = seed ^ stream.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    value = (value ^ (value >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    value ^ (value >> 31)
}

fn intent_name(intent: Intent) -> &'static str {
    match intent {
        Intent::Stack => "stack",
        Intent::SpikeNow => "spike_now",
        Intent::CancellationDodge => "cancellation_dodge",
        Intent::Counter => "counter",
        Intent::TankThenFire => "tank_then_fire",
        Intent::Charge => "charge",
        Intent::Dig => "dig",
        Intent::ComboContinue => "combo_continue",
        Intent::Survival => "survival",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strict_cheese_changes_hole_every_row() {
        let mut rng = StdRng::seed_from_u64(7);
        let field = cheese_field(12, true, &mut rng);
        let holes: Vec<_> = (0..12)
            .map(|y| (0..10).find(|&x| !field[y][x]).unwrap())
            .collect();
        assert!(holes.windows(2).all(|pair| pair[0] != pair[1]));
        assert!(field[..12]
            .iter()
            .all(|row| row.iter().filter(|&&cell| cell).count() == 9));
    }

    #[test]
    fn cancellation_consumes_oldest_packets_first() {
        let spec = PlayerSpec {
            agent: AgentKind::ColdClear,
            initial_field: InitialField::Empty,
        };
        let mut player = make_player(&spec, AgentConfig::default(), 1, 8).unwrap();
        player.incoming.push_back(GarbagePacket {
            lines: 2,
            arrival_ms: 10,
        });
        player.incoming.push_back(GarbagePacket {
            lines: 4,
            arrival_ms: 20,
        });
        assert_eq!(player.cancel_incoming(3), 3);
        assert_eq!(player.incoming.len(), 1);
        assert_eq!(player.incoming[0].lines, 3);
        assert_eq!(player.incoming[0].arrival_ms, 20);
    }

    #[test]
    fn grace_is_strictly_greater_than_one_second() {
        let spec = PlayerSpec {
            agent: AgentKind::ColdClear,
            initial_field: InitialField::Empty,
        };
        let mut player = make_player(&spec, AgentConfig::default(), 2, 8).unwrap();
        player.incoming.push_back(GarbagePacket {
            lines: 1,
            arrival_ms: 0,
        });
        let rules = Rules::pinned();
        assert_eq!(player.rise_matured(1000, &rules), 0);
        assert_eq!(player.rise_matured(1001, &rules), 1);
    }
}
