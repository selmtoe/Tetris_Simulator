#include "recovery.hpp"
#include "tetris_engine.hpp"

#include <fstream>
#include <algorithm>
#include <iostream>
#include <string>

using namespace tr;

namespace {

bool contains(const std::filesystem::path& path, const std::string& text) {
    std::ifstream file(path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return contents.find(text) != std::string::npos;
}

int base64Value(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

std::string decodeBase64(const std::string& encoded) {
    std::string decoded;
    int accumulator = 0;
    int bits = -8;
    for (char value : encoded) {
        if (value == '=') break;
        const int digit = base64Value(value);
        if (digit < 0) continue;
        accumulator = (accumulator << 6) | digit;
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return decoded;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "usage: recovery_output_test <existing-input> <output-dir>\n";
        return 2;
    }
    const std::filesystem::path input(argv[1]);
    const std::filesystem::path outputDirectory(argv[2]);

    TimelineStep initial;
    initial.startSeconds = 0;
    initial.timeSeconds = 0;
    initial.next = {Cell::T, Cell::S, Cell::Z, Cell::O, Cell::L};
    initial.board = {};
    initial.fullBoard = {};

    GarbageRise rise;
    rise.lines = 2;
    rise.holeMasks = {static_cast<std::uint16_t>(1u << 2), static_cast<std::uint16_t>(1u << 7)};
    rise.manuallySpecified = true;
    const Board base = TetrisEngine::applyGarbageRise(initial.board, rise);
    const auto moves = TetrisEngine::originalLegalMoves(base, Cell::T);
    if (moves.empty()) {
        std::cerr << "fixture has no legal T move\n";
        return 1;
    }

    // The middle state has T under control and only S..J in the visible
    // future queue. The exporter must prepend T for the simulator viewer.
    TimelineStep activeT = initial;
    activeT.startSeconds = .2;
    activeT.timeSeconds = .6;
    activeT.piece = Cell::T;
    activeT.hold = Cell::I;
    activeT.next = {Cell::S, Cell::Z, Cell::O, Cell::L, Cell::J};
    activeT.action = "place";
    activeT.holdUsed = true;

TimelineStep placed = activeT;
    placed.startSeconds = .6;
    placed.timeSeconds = .8;
    placed.piece = Cell::S;
    placed.next = {Cell::Z, Cell::O, Cell::L, Cell::J, Cell::I};
    placed.observed = moves.front().board;
    placed.board = moves.front().board;
    placed.fullBoard = moves.front().fullBoard;
    placed.garbage = rise;
    placed.placedPiece = moves.front().piece;
    placed.placementX = moves.front().x;
    placed.placementY = moves.front().y;
    placed.placementRotation = moves.front().rotation;
    placed.clearedLines = moves.front().clearedLines;
    placed.manuallyFixed = true;

    // The second player changes state while P1 is still on the same
    // highlighted-placement page. The combined replay must carry P1's
    // operation through that intermediate timestamp.
    TimelineStep p2Change = initial;
    p2Change.startSeconds = .4;
    p2Change.timeSeconds = .5;
    p2Change.piece = Cell::O;
    p2Change.next = {Cell::I, Cell::J, Cell::L, Cell::S, Cell::Z};

    Settings settings;
    RecoveryOutput output;
    output.p1 = {initial, activeT, placed};
    output.p2 = {initial, p2Change};
    QueueRecognitionSample queueSample;
    queueSample.timeSeconds = .123;
    queueSample.active = Cell::T;
    queueSample.observation.hold = Cell::I;
    queueSample.observation.next = {Cell::T, Cell::S, Cell::Z, Cell::O, Cell::L};
    queueSample.decoded = queueSample.observation;
    queueSample.stable = true;
    output.queueObservationsP1 = {queueSample};
    output.originalQueueObservationsP1 = output.queueObservationsP1;
    output.humanApproved = true;
    std::string error;
    if (!writeRecoveredOutput(input, outputDirectory, settings, output, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (!std::filesystem::exists(output.jsonPath) || !std::filesystem::exists(output.trainingAnnotationPath) ||
        !std::filesystem::exists(output.trainingVideoPath) || !std::filesystem::exists(output.trainingManifestPath) ||
        !std::filesystem::exists(output.combinedUrlPath) || !std::filesystem::exists(output.linksPath) ||
        !std::filesystem::exists(output.reportPath)) {
        std::cerr << "expected export files are missing\n";
        return 1;
    }
    if (!contains(output.trainingAnnotationPath, "tetris-video-recovery.training/v1") ||
        !contains(output.trainingAnnotationPath, "\"approvedByExplicitExport\":true") ||
        !contains(output.trainingAnnotationPath, "\"selfContained\":true") ||
        !contains(output.trainingAnnotationPath, "\"videoFile\":\"video/") ||
        !contains(output.trainingAnnotationPath, "\"holeMasks\":[4,128]") ||
        !contains(output.trainingAnnotationPath, "\"placement\"") ||
        !contains(output.trainingAnnotationPath, "\"queueObservations\"") ||
        !contains(output.trainingAnnotationPath, "\"time\":0.123") ||
        !contains(output.trainingAnnotationPath, "\"active\":\"T\"") ||
        !contains(output.trainingAnnotationPath, "\"decodedNext\":\"TSZOL\"")) {
        std::cerr << "training annotation is incomplete\n";
        return 1;
    }
    if (!contains(output.jsonPath, "\"garbage\"") || !contains(output.jsonPath, "\"placement\"") ||
        !contains(output.jsonPath, "\"queueObservations\"") ||
        !contains(output.jsonPath, "\"simulatorData\":{\"v\":3") ||
        !contains(output.jsonPath, "\"holdUsed\":true")) {
        std::cerr << "recovery JSON is missing structural labels\n";
        return 1;
    }
    if (!contains(output.linksPath, "<a href='") || !contains(output.linksPath, "Open 2P simulator")) {
        std::cerr << "simulator links page is incomplete\n";
        return 1;
    }
    const auto marker = output.p1Url.find('#');
    const std::string simulatorJson = marker == std::string::npos
        ? std::string()
        : decodeBase64(output.p1Url.substr(marker + 1));
    if (marker == std::string::npos || simulatorJson.find("\"v\":3") == std::string::npos ||
         simulatorJson.find("\"kind\":\"replay\"") == std::string::npos ||
         simulatorJson.find("\"o\":{\"type\":\"T\"") == std::string::npos ||
         simulatorJson.find("\"holdUsed\":true") == std::string::npos ||
         simulatorJson.find("\"sequence\":\"TSZOLJ") == std::string::npos ||
        simulatorJson.find("\"n\":\"TSZOLJ\"") == std::string::npos) {
        std::cerr << "simulator queue does not start with the active mino\n";
        return 1;
    }
    const auto combinedMarker = output.combinedUrl.find('#');
    const std::string combinedJson = combinedMarker == std::string::npos
        ? std::string()
        : decodeBase64(output.combinedUrl.substr(combinedMarker + 1));
    std::size_t coordinateMarkerCount = 0;
    for (std::size_t offset = 0;
         (offset = combinedJson.find("\"coordinateSpace\":\"simulator\"", offset)) != std::string::npos;
         offset += 1) {
        ++coordinateMarkerCount;
    }
    if (coordinateMarkerCount < 2) {
        std::cerr << "2P carried placement was dropped at the other player's page update\n";
        return 1;
    }

    RecoveryOutput reanalysis;
    reanalysis.videoDurationSeconds = .08;
    reanalysis.queueObservationsP1 = {
        {0.00, Cell::Empty, queueSample.observation, false, false},
        {0.01, Cell::T, queueSample.observation, true, false},
        {0.02, Cell::T, queueSample.observation, true, true},
    };
    reanalysis.originalQueueObservationsP1 = reanalysis.queueObservationsP1;
    Settings reanalysisSettings;
    std::string reanalysisError;
    if (!reanalyzeQueueObservations(reanalysisSettings, reanalysis, reanalysisError) ||
        reanalysis.rawP1.empty() || !std::any_of(reanalysis.rawP1.begin(), reanalysis.rawP1.end(),
                                                  [](const TimelineStep& step) { return step.queueManuallyFixed; }) ||
        !std::any_of(reanalysis.rawP1.begin(), reanalysis.rawP1.end(),
                     [](const TimelineStep& step) { return step.piece == Cell::T; })) {
        std::cerr << "raw queue log reanalysis failed: " << reanalysisError << '\n';
        return 1;
    }

    // A repeated but impossible visual-effect queue must not become a new
    // stable state. A later queue that is exactly two positions ahead is
    // accepted and creates one explicit skipped-piece phase.
    QueueObservation queueA;
    queueA.next = {Cell::I, Cell::O, Cell::T, Cell::J, Cell::L};
    QueueObservation impossible;
    impossible.next = {Cell::Z, Cell::Z, Cell::Z, Cell::S, Cell::S};
    QueueObservation queueB;
    queueB.next = {Cell::T, Cell::J, Cell::L, Cell::S, Cell::Z};
    RecoveryOutput transitionFixture;
    transitionFixture.videoDurationSeconds = .10;
    transitionFixture.queueObservationsP1 = {
        {0.00, Cell::Empty, queueA, false, false},
        {0.01, Cell::Empty, queueA, true, false},
        {0.02, Cell::Empty, impossible, false, false},
        {0.03, Cell::Empty, impossible, false, false},
        {0.04, Cell::Empty, queueA, true, false},
        {0.05, Cell::Empty, queueB, false, false},
        {0.06, Cell::Empty, queueB, true, false},
    };
    transitionFixture.originalQueueObservationsP1 = transitionFixture.queueObservationsP1;
    if (!reanalyzeQueueObservations(reanalysisSettings, transitionFixture, reanalysisError) ||
        !std::any_of(transitionFixture.rawP1.begin(), transitionFixture.rawP1.end(),
                     [](const TimelineStep& step) { return step.action == "spawn_skipped"; }) ||
        std::any_of(transitionFixture.rawP1.begin(), transitionFixture.rawP1.end(),
                    [](const TimelineStep& step) { return step.next == std::vector<Cell>{Cell::Z, Cell::Z, Cell::Z, Cell::S, Cell::S}; })) {
        std::cerr << "queue sliding plausibility filter failed: " << reanalysisError << '\n';
        return 1;
    }

    QueueObservation queueBefore;
    queueBefore.next = {Cell::L, Cell::O, Cell::J, Cell::S, Cell::I};
    QueueObservation queueAfterOne;
    queueAfterOne.next = {Cell::O, Cell::J, Cell::S, Cell::I, Cell::T};
    QueueObservation tailMisread;
    tailMisread.next = {Cell::O, Cell::J, Cell::S, Cell::I, Cell::O};
    RecoveryOutput tailFixture;
    tailFixture.videoDurationSeconds = .10;
    tailFixture.queueObservationsP1 = {
        {0.00, Cell::Empty, queueBefore, false, false},
        {0.01, Cell::Empty, queueBefore, true, false},
        {0.02, Cell::L, queueAfterOne, false, false},
        {0.03, Cell::L, queueAfterOne, true, false},
        {0.04, Cell::L, tailMisread, false, false},
        {0.05, Cell::L, tailMisread, true, false},
        {0.06, Cell::L, queueAfterOne, false, false},
        {0.07, Cell::L, queueAfterOne, true, false},
    };
    tailFixture.originalQueueObservationsP1 = tailFixture.queueObservationsP1;
    if (!reanalyzeQueueObservations(reanalysisSettings, tailFixture, reanalysisError) ||
        std::any_of(tailFixture.rawP1.begin(), tailFixture.rawP1.end(),
                    [](const TimelineStep& step) { return step.next == std::vector<Cell>{Cell::O, Cell::J, Cell::S, Cell::I, Cell::O}; }) ||
        !std::any_of(tailFixture.queueObservationsP1.begin(), tailFixture.queueObservationsP1.end(),
                     [](const QueueRecognitionSample& sample) {
                         return sample.sequenceCorrected &&
                                sample.observation.next == std::vector<Cell>{Cell::O, Cell::J, Cell::S, Cell::I, Cell::O} &&
                                sample.decoded.next == std::vector<Cell>{Cell::O, Cell::J, Cell::S, Cell::I, Cell::T};
                     })) {
        std::cerr << "tail-only queue misread was not globally corrected: " << reanalysisError << '\n';
        return 1;
    }
    int decodedSlides = 0;
    std::vector<Cell> lastDecoded;
    for (const auto& sample : tailFixture.queueObservationsP1) {
        if (sample.decoded.next.empty()) continue;
        if (!lastDecoded.empty() && sample.decoded.next != lastDecoded) ++decodedSlides;
        lastDecoded = sample.decoded.next;
    }
    const int lPhases = static_cast<int>(std::count_if(tailFixture.rawP1.begin(), tailFixture.rawP1.end(),
        [](const TimelineStep& step) { return step.piece == Cell::L; }));
    if (decodedSlides != 1 || lPhases != 1) {
        std::cerr << "tail misread invented an extra L placement\n";
        return 1;
    }

    // Match opening behaviour in VID_20260812_221204: the queue first
    // advances, then the first active T is put into an empty hold slot, and
    // the delayed visual queue slide exposes Z.  Z must survive as the
    // active piece of the following phase rather than disappearing between
    // the hold animation and the slide animation.
    QueueObservation opening0;
    opening0.next = {Cell::T, Cell::Z, Cell::O, Cell::I, Cell::J};
    QueueObservation opening1;
    opening1.next = {Cell::Z, Cell::O, Cell::I, Cell::J, Cell::L};
    QueueObservation openingHeld = opening1;
    openingHeld.hold = Cell::T;
    QueueObservation opening2;
    opening2.hold = Cell::T;
    opening2.next = {Cell::O, Cell::I, Cell::J, Cell::L, Cell::S};
    QueueObservation opening3;
    opening3.hold = Cell::T;
    opening3.next = {Cell::I, Cell::J, Cell::L, Cell::S, Cell::T};
    RecoveryOutput openingFixture;
    openingFixture.videoDurationSeconds = .10;
    openingFixture.queueObservationsP1 = {
        {0.00, Cell::Empty, opening0, false, false},
        {0.01, Cell::Empty, opening0, true, false},
        {0.02, Cell::Empty, opening1, false, false},
        {0.03, Cell::T, opening1, true, false},
        {0.04, Cell::T, openingHeld, false, false},
        {0.05, Cell::Empty, openingHeld, true, false},
        {0.06, Cell::Empty, opening2, false, false},
        {0.07, Cell::Z, opening2, true, false},
        {0.08, Cell::Z, opening3, false, false},
        {0.09, Cell::O, opening3, true, false},
    };
    openingFixture.originalQueueObservationsP1 = openingFixture.queueObservationsP1;
    if (!reanalyzeQueueObservations(reanalysisSettings, openingFixture, reanalysisError)) {
        std::cerr << "opening hold transition could not be reanalyzed: " << reanalysisError << '\n';
        return 1;
    }
    const auto heldZ = std::find_if(openingFixture.rawP1.begin(), openingFixture.rawP1.end(),
                                    [](const TimelineStep& step) { return step.piece == Cell::Z; });
    if (heldZ == openingFixture.rawP1.end() || !heldZ->holdUsed || heldZ->hold != Cell::T) {
        std::cerr << "opening hold transition lost Z:";
        for (const auto& step : openingFixture.rawP1) {
            std::cerr << ' ' << cellChar(step.piece) << '@' << step.timeSeconds
                      << (step.holdUsed ? ":hold" : "")
                      << ":stored=" << cellChar(step.hold)
                      << ":action=" << step.action;
        }
        std::cerr << '\n';
        return 1;
    }
    std::cout << "output export test passed\n";
    return 0;
}
