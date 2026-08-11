// Browser-facing C ABI for the bundled sfinder-cpp perfect-clear core.
// Input rows are 10-bit occupancy masks ordered from the floor upward.

#include <cstdint>
#include <vector>

#include "core/field.hpp"
#include "core/moves.hpp"
#include "core/piece.hpp"
#include "core/types.hpp"
#include "finder/perfect.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define PC_SOLVER_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define PC_SOLVER_EXPORT
#endif

namespace {
constexpr int kMaxHeight = core::MAX_FIELD_HEIGHT;
constexpr int kOperationStride = 12;

bool isValidPiece(const std::uint8_t piece) {
    return piece <= static_cast<std::uint8_t>(core::PieceType::O);
}

bool isEmpty(const core::Field &field) {
    return field == core::Field{};
}
}

// Return values:
//   > 0: number of operations written to outOperations
//     0: no perfect clear for this known sequence
//    -1: invalid input
//    -2: output buffer is too small
//
// One operation occupies 12 int32 values:
// [piece, rotation, pivotX, pivotY, x0, y0, x1, y1, x2, y2, x3, y3]
// Coordinates use the solver's bottom-up board origin.  The JS Worker turns
// them into the simulator's top-down 40-row coordinate system.
extern "C" PC_SOLVER_EXPORT int sfinder_find_pc(
        const std::uint16_t *rowsBottomUp,
        const int rowCount,
        const std::uint8_t *pieces,
        const int pieceCount,
        const int maxDepth,
        const int maxLines,
        const int holdEmpty,
        std::int32_t *outOperations,
        const int operationCapacity
) {
    if (!rowsBottomUp || !pieces || !outOperations || rowCount < 1 || rowCount > kMaxHeight ||
        pieceCount < 1 || maxDepth < 1 || maxDepth > pieceCount || maxLines < 1 || maxLines > kMaxHeight ||
        operationCapacity < maxDepth) {
        return -1;
    }

    core::Field field;
    for (int y = 0; y < rowCount; ++y) {
        const std::uint16_t row = rowsBottomUp[y];
        if ((row & ~static_cast<std::uint16_t>(0x03ff)) != 0) {
            return -1;
        }
        for (int x = 0; x < core::FIELD_WIDTH; ++x) {
            if ((row & (static_cast<std::uint16_t>(1) << x)) != 0) {
                field.setBlock(x, y);
            }
        }
    }

    std::vector<core::PieceType> sequence;
    sequence.reserve(pieceCount);
    for (int index = 0; index < pieceCount; ++index) {
        if (!isValidPiece(pieces[index])) {
            return -1;
        }
        sequence.push_back(static_cast<core::PieceType>(pieces[index]));
    }

    const auto factory = core::Factory::create();
    auto moveGenerator = core::srs::MoveGenerator(factory);
    auto perfectFinder = finder::PerfectFinder<core::srs::MoveGenerator>(factory, moveGenerator);
    const auto solution = perfectFinder.run(field, sequence, maxDepth, maxLines, holdEmpty != 0);
    if (solution.empty()) {
        return 0;
    }

    // Replaying makes the ABI defensive even if the upstream search changes:
    // only an actually empty final field is reported as a PC.
    auto replay = field;
    int operationCount = 0;
    for (const auto &operation : solution) {
        if (operation.x < 0 || operation.y < 0) {
            break;
        }
        if (operationCount >= operationCapacity) {
            return -2;
        }

        const auto &blocks = factory.get(operation.pieceType, operation.rotateType);
        const int offset = operationCount * kOperationStride;
        outOperations[offset] = static_cast<std::int32_t>(operation.pieceType);
        outOperations[offset + 1] = static_cast<std::int32_t>(operation.rotateType);
        outOperations[offset + 2] = operation.x;
        outOperations[offset + 3] = operation.y;
        for (int cell = 0; cell < 4; ++cell) {
            outOperations[offset + 4 + cell * 2] = operation.x + blocks.points[cell].x;
            outOperations[offset + 5 + cell * 2] = operation.y + blocks.points[cell].y;
        }

        replay.put(blocks, operation.x, operation.y);
        replay.clearLine();
        ++operationCount;
    }

    if (operationCount != maxDepth || !isEmpty(replay)) {
        return 0;
    }

    return operationCount;
}
