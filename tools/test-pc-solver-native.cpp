#include <array>
#include <cstdint>
#include <iostream>
#include <string>

extern "C" int sfinder_find_pc(
        const std::uint16_t *rowsBottomUp,
        int rowCount,
        const std::uint8_t *pieces,
        int pieceCount,
        int maxDepth,
        int maxLines,
        int holdEmpty,
        std::int32_t *outOperations,
        int operationCapacity
);

namespace {
std::uint16_t rowMask(const std::string &row) {
    std::uint16_t mask = 0;
    for (int x = 0; x < 10; ++x) {
        if (row.at(x) != '_') mask |= static_cast<std::uint16_t>(1u << x);
    }
    return mask;
}
}

int main() {
    // sfinder-cpp's bundled sample: 24 existing blocks plus 9 placed pieces
    // make six cleared rows. The first mino is the occupied hold slot.
    std::array<std::uint16_t, 24> rows{};
    rows[0] = rowMask("XXXXXX_XXX");
    rows[1] = rowMask("_XXXXXXXXX");
    rows[2] = rowMask("_XXXXXX___");

    // PieceType order is T, I, L, J, S, Z, O.
    const std::array<std::uint8_t, 10> pieces{1, 3, 3, 4, 6, 2, 5, 0, 1, 5};
    std::array<std::int32_t, 9 * 12> operations{};

    const int count = sfinder_find_pc(
            rows.data(), static_cast<int>(rows.size()), pieces.data(), static_cast<int>(pieces.size()),
            9, 6, false, operations.data(), 9
    );
    if (count != 9) {
        std::cerr << "Expected a 9-step PC solution, got " << count << '\n';
        return 1;
    }

    for (int operation = 0; operation < count; ++operation) {
        const int offset = operation * 12;
        for (int cell = 0; cell < 4; ++cell) {
            const int x = operations[offset + 4 + cell * 2];
            const int y = operations[offset + 5 + cell * 2];
            if (x < 0 || x >= 10 || y < 0 || y >= 24) {
                std::cerr << "Invalid guide cell in operation " << operation << '\n';
                return 1;
            }
        }
    }

    std::cout << "PC wrapper smoke test passed: " << count << " operations\n";
    return 0;
}
