#include "tetris_engine.hpp"

#include <iostream>
#include <algorithm>
#include <set>
#include <array>

using namespace tr;

int main() {
    Board empty{};
    constexpr std::array<int, 7> sourceMoveCounts{17, 34, 9, 17, 34, 34, 17};
    for (std::size_t pieceIndex = 0; pieceIndex < Pieces.size(); ++pieceIndex) {
        const Cell piece = Pieces[pieceIndex];
        const auto moves = TetrisEngine::legalMoves(empty, piece);
        if (moves.empty()) { std::cerr << "no legal moves for " << cellChar(piece) << '\n'; return 1; }
        std::set<std::string> unique;
        for (const auto& move : moves) {
            int occupied = 0;
            for (Cell cell : move.board) if (cell != Cell::Empty) ++occupied;
            if (occupied != 4) { std::cerr << "bad block count for " << cellChar(piece) << '\n'; return 1; }
            std::string key(reinterpret_cast<const char*>(move.board.data()), move.board.size());
            unique.insert(key);
        }
        if (unique.size() != moves.size()) { std::cerr << "duplicate move for " << cellChar(piece) << '\n'; return 1; }
        std::cout << cellChar(piece) << ": " << moves.size() << " reachable lock states\n";

        // The recovery beam intentionally uses the legacy generator from
        // 動画解析.html. Keep its candidate universe pinned separately from
        // the simulator-SRS reachability implementation above.
        const auto sourceMoves = TetrisEngine::originalLegalMoves(empty, piece);
        if (sourceMoves.size() != static_cast<std::size_t>(sourceMoveCounts[pieceIndex])) {
            std::cerr << "source-compatible move count changed for " << cellChar(piece)
                      << ": " << sourceMoves.size() << '\n';
            return 1;
        }
        for (const auto& move : sourceMoves) {
            int occupied = 0;
            for (Cell cell : move.fullBoard) if (cell != Cell::Empty) ++occupied;
            if (occupied != 4) { std::cerr << "bad source block count for " << cellChar(piece) << '\n'; return 1; }
        }
    }

    Board lineClear{};
    for (int x = 0; x < 6; ++x) lineClear[index(x, 39)] = Cell::Garbage;
    const auto moves = TetrisEngine::legalMoves(lineClear, Cell::I);
    bool foundClear = false;
    for (const auto& move : moves) if (move.clearedLines == 1) foundClear = true;
    if (!foundClear) { std::cerr << "line clear candidate missing\n"; return 1; }

    // Manual correction must enumerate the same candidates as the recovery
    // beam, then hold the selected board fixed while only the later phases
    // are recomputed.
    Settings settings;
    std::vector<TimelineStep> raw(4);
    raw[0].observed = Board{};
    raw[0].board = raw[0].observed;
    raw[0].fullBoard = raw[0].observed;
    raw[0].piece = Cell::Empty;

    raw[1] = raw[0];
    raw[1].piece = Cell::I;

    const auto iMoves = TetrisEngine::originalLegalMoves(Board{}, Cell::I);
    raw[2] = raw[1];
    raw[2].observed = iMoves.front().board;
    raw[2].board = raw[2].observed;
    raw[2].fullBoard = raw[2].observed;
    raw[2].piece = Cell::O;

    const auto alternative = iMoves.back();
    const auto oMoves = TetrisEngine::originalLegalMoves(alternative.board, Cell::O);
    if (oMoves.empty()) { std::cerr << "missing O correction tail move\n"; return 1; }
    raw[3] = raw[2];
    raw[3].observed = oMoves.front().board;
    raw[3].board = raw[3].observed;
    raw[3].fullBoard = raw[3].observed;

    const auto automatic = TetrisEngine::beamSearch(raw, settings);
    const auto choices = TetrisEngine::correctionCandidates(raw, automatic, 2, settings);
    if (choices.size() != iMoves.size()) { std::cerr << "manual correction candidates do not match I moves\n"; return 1; }
    const auto selected = std::find_if(choices.begin(), choices.end(), [&](const CorrectionCandidate& candidate) {
        return candidate.move.board == alternative.board && candidate.move.fullBoard == alternative.fullBoard;
    });
    if (selected == choices.end()) { std::cerr << "selected legal candidate missing\n"; return 1; }

    raw[2].manuallyFixed = true;
    raw[2].board = selected->move.board;
    raw[2].fullBoard = selected->move.fullBoard;
    raw[2].garbage = selected->garbage;
    raw[2].placedPiece = selected->move.piece;
    raw[2].placementX = selected->move.x;
    raw[2].placementY = selected->move.y;
    raw[2].placementRotation = selected->move.rotation;
    raw[2].clearedLines = selected->move.clearedLines;
    const auto repaired = TetrisEngine::recomputeFrom(raw, automatic, 2, settings);
    if (repaired[2].board != alternative.board || repaired[2].fullBoard != alternative.fullBoard) {
        std::cerr << "manual correction was not preserved\n";
        return 1;
    }
    if (repaired[3].board != oMoves.front().board) {
        std::cerr << "manual correction tail was not recomputed\n";
        return 1;
    }

    // An exact legal 20x10 ONNX match is a hard transition constraint. Use
    // intentionally perverse weights to prove this is not merely a normal
    // cumulative-score preference.
    Settings forceExactSettings;
    forceExactSettings.forceExactOnnxMatch = true;
    forceExactSettings.weights.exactMatch = -100.0;
    forceExactSettings.weights.emptyMatch = -100.0;
    forceExactSettings.weights.colorMismatch = 100.0;
    forceExactSettings.weights.missingBlock = 100.0;
    forceExactSettings.weights.extraBlock = 100.0;
    std::vector<TimelineStep> exactRaw(3);
    exactRaw[0].observed = Board{};
    exactRaw[0].board = exactRaw[0].observed;
    exactRaw[0].fullBoard = exactRaw[0].observed;
    exactRaw[0].piece = Cell::Empty;
    exactRaw[1] = exactRaw[0];
    exactRaw[1].piece = Cell::I;
    const CandidateMove exactI = iMoves[5];
    exactRaw[2] = exactRaw[1];
    exactRaw[2].observed = exactI.board;
    exactRaw[2].board = exactI.board;
    exactRaw[2].fullBoard = exactI.board;
    exactRaw[2].piece = Cell::O;
    const auto forcedExact = TetrisEngine::beamSearch(exactRaw, forceExactSettings);
    if (forcedExact.size() != exactRaw.size() || forcedExact[2].board != exactI.board) {
        std::cerr << "exact ONNX legal move was not forced\n";
        return 1;
    }

    // A missed incoming garbage animation must remain repairable without an
    // ONNX observation.  The explicit masks are gaps, not colors copied from
    // an observed board, and legal candidates are generated from that base.
    Board beforeGarbage{};
    beforeGarbage[index(1, 37)] = Cell::T;
    GarbageRise manualRise;
    manualRise.lines = 2;
    manualRise.holeMasks = {static_cast<std::uint16_t>(1u << 2), static_cast<std::uint16_t>(1u << 7)};
    manualRise.manuallySpecified = true;
    const Board afterGarbage = TetrisEngine::applyGarbageRise(beforeGarbage, manualRise);
    if (afterGarbage[index(1, 35)] != Cell::T ||
        afterGarbage[index(2, 38)] != Cell::Empty || afterGarbage[index(7, 39)] != Cell::Empty ||
        afterGarbage[index(1, 38)] != Cell::Garbage || afterGarbage[index(1, 39)] != Cell::Garbage) {
        std::cerr << "manual garbage rise shape is wrong\n";
        return 1;
    }
    // Even malformed input with several apparent holes must become exactly
    // one gap in each garbage row. The UI uses the same invariant when a
    // reviewer clicks a new hole column.
    GarbageRise malformedRise = manualRise;
    malformedRise.lines = 1;
    malformedRise.holeMasks = {static_cast<std::uint16_t>((1u << 2) | (1u << 7))};
    const Board normalizedGarbage = TetrisEngine::applyGarbageRise(Board{}, malformedRise);
    if (normalizedGarbage[index(2, 39)] != Cell::Empty || normalizedGarbage[index(7, 39)] != Cell::Garbage) {
        std::cerr << "multi-hole garbage was not normalized to one hole\n";
        return 1;
    }
    const auto garbageChoices = TetrisEngine::correctionCandidates(raw, automatic, 2, settings, manualRise);
    if (garbageChoices.empty() || !garbageChoices.front().garbage.manuallySpecified ||
        garbageChoices.front().garbage.lines != 2 || garbageChoices.front().garbage.holeMasks != manualRise.holeMasks) {
        std::cerr << "manual garbage rise did not feed legal candidate generation\n";
        return 1;
    }

    std::cout << "engine tests passed\n";
    return 0;
}
