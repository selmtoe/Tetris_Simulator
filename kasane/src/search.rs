use libtetris::{
    find_moves, Board, FallingPiece, LockResult, MovementMode, Piece, PieceMovement, SpawnRule,
    COMBO_GARBAGE,
};
use serde::{Deserialize, Serialize};
use std::collections::HashSet;

#[derive(Clone, Debug)]
pub struct PlacementAction {
    pub placement: FallingPiece,
    pub hold: bool,
    pub movements: Vec<PieceMovement>,
    pub board_after: Board,
    pub lock: LockResult,
    pub pieces_consumed: usize,
}

impl PlacementAction {
    pub fn key(&self) -> ActionKey {
        ActionKey {
            placement: self.placement,
            hold: self.hold,
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct ActionKey {
    pub placement: FallingPiece,
    pub hold: bool,
}

/// Enumerate every reachable root placement, preserving the queue/HOLD state
/// that follows the placement. This is deliberately independent from Cold
/// Clear's evaluator so tactical branches cannot disappear during reranking.
pub fn legal_actions(board: &Board) -> Vec<PlacementAction> {
    let mut root = board.clone();
    let current = match root.advance_queue() {
        Some(piece) => piece,
        None => return Vec::new(),
    };

    let mut actions = Vec::with_capacity(96);
    add_piece_actions(&mut actions, &root, current, false, 1);

    let mut held_board = root;
    let held_piece = match held_board.hold(current) {
        Some(piece) => Some((piece, 1)),
        None => held_board.advance_queue().map(|piece| (piece, 2)),
    };
    if let Some((piece, consumed)) = held_piece {
        if piece != current || consumed == 2 {
            add_piece_actions(&mut actions, &held_board, piece, true, consumed);
        }
    }

    // Some SRS paths canonicalize to the same physical result. Keep the
    // shortest controller sequence for deterministic timing.
    let mut seen = HashSet::with_capacity(actions.len());
    actions.sort_by_key(|action| action.movements.len());
    actions.retain(|action| seen.insert(action.key()));
    actions
}

fn add_piece_actions(
    output: &mut Vec<PlacementAction>,
    board: &Board,
    piece: Piece,
    hold: bool,
    pieces_consumed: usize,
) {
    let spawned = match SpawnRule::Row19Or20.spawn(piece, board) {
        Some(piece) => piece,
        None => return,
    };
    for placement in find_moves(board, spawned, MovementMode::ZeroGComplete) {
        let mut board_after = board.clone();
        let lock = board_after.lock_piece(placement.location);
        output.push(PlacementAction {
            placement: placement.location,
            hold,
            movements: placement.inputs.movements.into_iter().collect(),
            board_after,
            lock,
            pieces_consumed,
        });
    }
}

/// Attack under PC0: a perfect clear receives no special override, but the
/// underlying clear, B2B and REN damage remain intact.
pub fn pc0_attack(lock: &LockResult) -> u32 {
    let mut attack = lock.placement_kind.garbage();
    if lock.b2b {
        attack += 1;
    }
    if let Some(combo) = lock.combo {
        attack += COMBO_GARBAGE[combo.min((COMBO_GARBAGE.len() - 1) as u32) as usize];
    }
    attack
}

/// Attack under the active match rules. A positive perfect-clear value is an
/// override, matching the simulator; zero keeps the ordinary clear/B2B/REN
/// damage used by the PC0 training benchmark.
pub fn attack_with_pc(lock: &LockResult, perfect_clear_special_attack: u32) -> u32 {
    if lock.perfect_clear && perfect_clear_special_attack > 0 {
        perfect_clear_special_attack
    } else {
        pc0_attack(lock)
    }
}

pub fn same_action(left: &PlacementAction, placement: FallingPiece, hold: bool) -> bool {
    left.hold == hold && left.placement.same_location(&placement)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn board_with_queue(sequence: &[Piece]) -> Board {
        let mut board = Board::new();
        for &piece in sequence {
            board.add_next_piece(piece);
        }
        board
    }

    #[test]
    fn empty_board_has_reachable_actions() {
        let board = board_with_queue(&[
            Piece::I,
            Piece::O,
            Piece::T,
            Piece::S,
            Piece::Z,
            Piece::L,
            Piece::J,
            Piece::I,
            Piece::O,
        ]);
        let actions = legal_actions(&board);
        assert!(actions.len() >= 20, "only {} actions", actions.len());
        assert!(actions.iter().any(|action| action.hold));
        assert!(actions.iter().any(|action| !action.hold));
    }

    #[test]
    fn pc0_keeps_normal_clear_damage() {
        let mut lock = LockResult::default();
        lock.placement_kind = libtetris::PlacementKind::Clear4;
        lock.perfect_clear = true;
        lock.garbage_sent = 10;
        assert_eq!(pc0_attack(&lock), 4);
        assert_eq!(attack_with_pc(&lock, 10), 10);
    }
}
