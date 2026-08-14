//! Thin C ABI around the original Cold Clear Standard bot.
//!
//! The algorithmic modules in this crate are taken from the supplied
//! `cold-clear-master.zip` reference source.  This file only supplies the
//! browser-facing lifecycle and converts the simulator's top-to-bottom board
//! coordinates to libtetris' bottom-to-top coordinates.

#![allow(clippy::missing_safety_doc)]

#[path = "../../../third_party/cold-clear-reference/bot/src/dag.rs"]
mod dag;
#[path = "../../../third_party/cold-clear-reference/bot/src/evaluation/mod.rs"]
pub mod evaluation;
#[path = "../../../third_party/cold-clear-reference/bot/src/modes/normal.rs"]
mod normal;

pub use libtetris::*;
pub use normal::{BotState, ThinkResult, Thinker};
use opening_book::Book;
use serde::{Deserialize, Serialize};
use std::slice;

#[derive(Copy, Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct Options {
    pub mode: MovementMode,
    pub spawn_rule: SpawnRule,
    pub use_hold: bool,
    pub speculate: bool,
    pub min_nodes: u32,
    pub max_nodes: u32,
    pub threads: u32,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            mode: MovementMode::ZeroG,
            spawn_rule: SpawnRule::Row19Or20,
            use_hold: true,
            speculate: true,
            min_nodes: 0,
            max_nodes: 4_000_000_000,
            threads: 1,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq, Hash)]
pub enum Info {
    Normal(normal::Info),
    Book,
}

impl Info {
    pub fn plan(&self) -> &[(FallingPiece, LockResult)] {
        match self {
            Self::Normal(info) => &info.plan,
            Self::Book => &[],
        }
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct CcWeights {
    pub back_to_back: i32,
    pub bumpiness: i32,
    pub bumpiness_sq: i32,
    pub row_transitions: i32,
    pub height: i32,
    pub top_half: i32,
    pub top_quarter: i32,
    pub jeopardy: i32,
    pub cavity_cells: i32,
    pub cavity_cells_sq: i32,
    pub overhang_cells: i32,
    pub overhang_cells_sq: i32,
    pub covered_cells: i32,
    pub covered_cells_sq: i32,
    pub tslot: [i32; 4],
    pub well_depth: i32,
    pub max_well_depth: i32,
    pub well_column: [i32; 10],
    pub b2b_clear: i32,
    pub clear1: i32,
    pub clear2: i32,
    pub clear3: i32,
    pub clear4: i32,
    pub tspin1: i32,
    pub tspin2: i32,
    pub tspin3: i32,
    pub mini_tspin1: i32,
    pub mini_tspin2: i32,
    pub perfect_clear: i32,
    pub combo_garbage: i32,
    pub move_time: i32,
    pub wasted_t: i32,
    pub use_bag: bool,
    pub timed_jeopardy: bool,
    pub stack_pc_damage: bool,
}

impl From<CcWeights> for evaluation::Standard {
    fn from(w: CcWeights) -> Self {
        Self {
            back_to_back: w.back_to_back,
            bumpiness: w.bumpiness,
            bumpiness_sq: w.bumpiness_sq,
            row_transitions: w.row_transitions,
            height: w.height,
            top_half: w.top_half,
            top_quarter: w.top_quarter,
            jeopardy: w.jeopardy,
            cavity_cells: w.cavity_cells,
            cavity_cells_sq: w.cavity_cells_sq,
            overhang_cells: w.overhang_cells,
            overhang_cells_sq: w.overhang_cells_sq,
            covered_cells: w.covered_cells,
            covered_cells_sq: w.covered_cells_sq,
            tslot: w.tslot,
            well_depth: w.well_depth,
            max_well_depth: w.max_well_depth,
            well_column: w.well_column,
            b2b_clear: w.b2b_clear,
            clear1: w.clear1,
            clear2: w.clear2,
            clear3: w.clear3,
            clear4: w.clear4,
            tspin1: w.tspin1,
            tspin2: w.tspin2,
            tspin3: w.tspin3,
            mini_tspin1: w.mini_tspin1,
            mini_tspin2: w.mini_tspin2,
            perfect_clear: w.perfect_clear,
            combo_garbage: w.combo_garbage,
            move_time: w.move_time,
            wasted_t: w.wasted_t,
            use_bag: w.use_bag,
            timed_jeopardy: w.timed_jeopardy,
            stack_pc_damage: w.stack_pc_damage,
            sub_name: None,
        }
    }
}

#[repr(C)]
pub struct CcMove {
    pub status: u32,
    pub piece: u8,
    pub hold: u8,
    pub rotation: u8,
    pub tspin: u8,
    pub x: i32,
    pub y: i32,
    pub movement_count: u8,
    pub movements: [u8; 32],
    pub nodes: u32,
    pub depth: u32,
}

impl Default for CcMove {
    fn default() -> Self {
        Self {
            status: 0,
            piece: 0,
            hold: 0,
            rotation: 0,
            tspin: 0,
            x: 0,
            y: 0,
            movement_count: 0,
            movements: [0; 32],
            nodes: 0,
            depth: 0,
        }
    }
}

pub struct CcBot {
    state: BotState<evaluation::Standard>,
    evaluator: evaluation::Standard,
    board: Board,
    pending: Option<FallingPiece>,
}

fn piece_from_byte(value: u8) -> Option<Piece> {
    match value {
        b'I' => Some(Piece::I),
        b'O' => Some(Piece::O),
        b'T' => Some(Piece::T),
        b'L' => Some(Piece::L),
        b'J' => Some(Piece::J),
        b'S' => Some(Piece::S),
        b'Z' => Some(Piece::Z),
        _ => None,
    }
}

fn piece_to_byte(piece: Piece) -> u8 {
    match piece {
        Piece::I => b'I',
        Piece::O => b'O',
        Piece::T => b'T',
        Piece::L => b'L',
        Piece::J => b'J',
        Piece::S => b'S',
        Piece::Z => b'Z',
    }
}

fn field_from_simulator(board: &[u8]) -> [[bool; 10]; 40] {
    let mut field = [[false; 10]; 40];
    for source_y in 0..40 {
        let simulator_y = 39 - source_y;
        for x in 0..10 {
            field[source_y][x] = board[simulator_y * 10 + x] != 0;
        }
    }
    field
}

fn make_board(
    board_data: &[u8],
    current: u8,
    next: &[u8],
    hold: u8,
    can_hold: bool,
    b2b: bool,
    ren: i32,
) -> Board {
    let mut board = Board::new();
    board.set_field(field_from_simulator(board_data));
    board.b2b_bonus = b2b;
    board.combo = (ren + 1).max(0) as u32;
    if let Some(piece) = piece_from_byte(hold) {
        board.hold_piece = Some(piece);
    }
    if let Some(piece) = piece_from_byte(current) {
        board.add_next_piece(piece);
    }
    for &value in next {
        if let Some(piece) = piece_from_byte(value) {
            board.add_next_piece(piece);
        }
    }
    let _ = can_hold;
    board
}

fn rotation_number(rotation: RotationState) -> u8 {
    match rotation {
        RotationState::North => 0,
        RotationState::East => 1,
        RotationState::South => 2,
        RotationState::West => 3,
    }
}

fn movement_byte(movement: PieceMovement) -> u8 {
    match movement {
        PieceMovement::Left => b'L',
        PieceMovement::Right => b'R',
        PieceMovement::Cw => b'C',
        PieceMovement::Ccw => b'V',
        PieceMovement::SonicDrop => b'D',
    }
}

fn output_move(result: &mut CcMove, mv: &Move, info: &Info) {
    result.status = 1;
    result.piece = piece_to_byte(mv.expected_location.kind.0);
    result.hold = mv.hold as u8;
    result.rotation = rotation_number(mv.expected_location.kind.1);
    result.tspin = match mv.expected_location.tspin {
        TspinStatus::None => 0,
        TspinStatus::Mini => 1,
        TspinStatus::Full => 2,
    };
    // Source coordinates are bottom-up. The simulator uses top-down rows.
    result.x = mv.expected_location.x - (mv.expected_location.kind.0 == Piece::I) as i32;
    result.y = 39 - mv.expected_location.y;
    result.movement_count = mv.inputs.len().min(32) as u8;
    for (i, &movement) in mv.inputs.iter().take(32).enumerate() {
        result.movements[i] = movement_byte(movement);
    }
    if let Info::Normal(normal) = info {
        result.nodes = normal.nodes;
        result.depth = normal.depth;
    }
}

fn make_options(max_nodes: u32) -> Options {
    Options {
        mode: MovementMode::ZeroG,
        spawn_rule: SpawnRule::Row19Or20,
        use_hold: true,
        speculate: true,
        min_nodes: 0,
        max_nodes: max_nodes.max(1),
        threads: 1,
    }
}

fn parse_weights(ptr: *const u8, len: usize) -> evaluation::Standard {
    if ptr.is_null() || len == 0 {
        return evaluation::Standard::default();
    }
    let bytes = unsafe { slice::from_raw_parts(ptr, len) };
    let mut defaults = match serde_json::to_value(evaluation::Standard::default()) {
        Ok(value) => value,
        Err(_) => return evaluation::Standard::default(),
    };
    let overrides = match serde_json::from_slice::<serde_json::Value>(bytes) {
        Ok(value) => value,
        Err(_) => return evaluation::Standard::default(),
    };
    if let (Some(defaults), Some(overrides)) = (defaults.as_object_mut(), overrides.as_object()) {
        for (key, value) in overrides {
            defaults.insert(key.clone(), value.clone());
        }
    }
    serde_json::from_value::<CcWeights>(defaults)
        .map(Into::into)
        .unwrap_or_else(|_| evaluation::Standard::default())
}

unsafe fn make_bot(
    board: *const u8,
    current: u8,
    next: *const u8,
    next_count: u32,
    hold: u8,
    can_hold: u8,
    b2b: u8,
    ren: i32,
    max_nodes: u32,
    weights: *const u8,
    weights_len: u32,
) -> *mut CcBot {
    if board.is_null() || (next_count != 0 && next.is_null()) {
        return std::ptr::null_mut();
    }
    let board_data = slice::from_raw_parts(board, 400);
    let next_data = slice::from_raw_parts(next, next_count as usize);
    let board_state = make_board(
        board_data,
        current,
        next_data,
        hold,
        can_hold != 0,
        b2b != 0,
        ren,
    );
    let options = make_options(max_nodes);
    let evaluator = parse_weights(weights, weights_len as usize);
    let state = BotState::new(board_state.clone(), options);
    Box::into_raw(Box::new(CcBot {
        state,
        evaluator,
        board: board_state,
        pending: None,
    }))
}

#[no_mangle]
pub unsafe extern "C" fn cc_create(
    board: *const u8,
    current: u8,
    next: *const u8,
    next_count: u32,
    hold: u8,
    can_hold: u8,
    b2b: u8,
    ren: i32,
    max_nodes: u32,
    weights: *const u8,
    weights_len: u32,
) -> *mut CcBot {
    make_bot(
        board,
        current,
        next,
        next_count,
        hold,
        can_hold,
        b2b,
        ren,
        max_nodes,
        weights,
        weights_len,
    )
}

#[no_mangle]
pub unsafe extern "C" fn cc_think(bot: *mut CcBot, iterations: u32) -> u32 {
    if bot.is_null() {
        return 0;
    }
    let bot = &mut *bot;
    let mut completed = 0;
    while completed < iterations {
        let thinker = match bot.state.think() {
            Ok(thinker) => thinker,
            Err(_) => break,
        };
        let result = thinker.think(&bot.evaluator);
        bot.state.finish_thinking(result);
        completed += 1;
    }
    bot.state.node_count()
}

#[no_mangle]
pub unsafe extern "C" fn cc_suggest(
    bot: *mut CcBot,
    incoming: u32,
    result: *mut CcMove,
) -> u32 {
    if bot.is_null() || result.is_null() {
        return 0;
    }
    let bot = &mut *bot;
    let output = &mut *result;
    *output = CcMove::default();
    if let Some((mv, info)) = bot.state.suggest_move(&bot.evaluator, None::<&Book>, incoming) {
        bot.pending = Some(mv.expected_location);
        output_move(output, &mv, &info);
        1
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn cc_commit(bot: *mut CcBot) -> u32 {
    if bot.is_null() {
        return 0;
    }
    let bot = &mut *bot;
    let mv = match bot.pending.take() {
        Some(mv) => mv,
        None => return 0,
    };
    bot.state.advance_move(mv);

    // Keep a mirror of the reference bot's public board lifecycle so the
    // next preview and the next snapshot can be synchronized without losing
    // the DAG.
    let next = match bot.board.advance_queue() {
        Some(piece) => piece,
        None => return 0,
    };
    if mv.kind.0 != next {
        if bot.board.hold(next).is_none() {
            bot.board.advance_queue();
        }
    }
    bot.board.lock_piece(mv);
    1
}

#[no_mangle]
pub unsafe extern "C" fn cc_add_next_piece(bot: *mut CcBot, piece: u8) -> u32 {
    if bot.is_null() {
        return 0;
    }
    let bot = &mut *bot;
    let piece = match piece_from_byte(piece) {
        Some(piece) => piece,
        None => return 0,
    };
    bot.state.add_next_piece(piece);
    bot.board.add_next_piece(piece);
    1
}

#[no_mangle]
pub unsafe extern "C" fn cc_node_count(bot: *const CcBot) -> u32 {
    if bot.is_null() {
        0
    } else {
        (*bot).state.node_count()
    }
}

#[no_mangle]
pub unsafe extern "C" fn cc_destroy(bot: *mut CcBot) {
    if !bot.is_null() {
        drop(Box::from_raw(bot));
    }
}

#[no_mangle]
pub unsafe extern "C" fn cc_alloc(size: usize) -> *mut u8 {
    if size == 0 {
        return std::ptr::null_mut();
    }
    let layout = std::alloc::Layout::from_size_align(size, 8).unwrap();
    std::alloc::alloc_zeroed(layout)
}

#[no_mangle]
pub unsafe extern "C" fn cc_dealloc(ptr: *mut u8, size: usize) {
    if ptr.is_null() || size == 0 {
        return;
    }
    let layout = std::alloc::Layout::from_size_align(size, 8).unwrap();
    std::alloc::dealloc(ptr, layout);
}

#[no_mangle]
pub extern "C" fn cc_move_size() -> usize {
    std::mem::size_of::<CcMove>()
}
