#include "recovery.hpp"

#include "onnx_model.hpp"
#include "tetris_engine.hpp"
#include "vision.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <ostream>
#include <sstream>

namespace tr {

namespace {

bool samePieces(const std::vector<Cell>& a, const std::vector<Cell>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool sameQueue(const QueueObservation& a, const QueueObservation& b) {
    return a.hold == b.hold && samePieces(a.next, b.next);
}

bool validQueue(const QueueObservation& queue) {
    if (queue.next.size() < 3) return false;
    // Keep accepting shorter simulator/video queue windows, but never allow a
    // non-piece in any slot that was actually observed.  Checking only the
    // first three slots would let a corrupted tail participate in a later
    // transition and make the decoder invent a placement.
    return std::all_of(queue.next.begin(), queue.next.end(), [](Cell cell) {
        return isPiece(cell);
    });
}

struct QueueTransition {
    std::string action = "none";
    bool nextAdvanced = false;
    bool holdChanged = false;
    bool plausible = false;
    int slideCount = 0;
};

QueueTransition checkQueueTransition(const QueueObservation& observed,
                                     const QueueObservation& last,
                                     Cell active) {
    // The queue is a sliding window. A real lock can advance it by one, and
    // two locks can be hidden between two video scans, but an unrelated
    // random 5-piece string cannot suddenly replace it. The old implementation
    // only checked one slide and accepted any confirmed value; that allowed
    // visual effects to permanently poison the queue state.
    QueueTransition result;
    if (!validQueue(observed) || !validQueue(last)) return result;
    result.holdChanged = observed.hold != last.hold;

    const auto overlapMatches = [&](int offset, int required) {
        const int available = std::min(static_cast<int>(observed.next.size()),
                                       static_cast<int>(last.next.size()) - offset);
        if (available < required) return false;
        for (int i = 0; i < required; ++i) {
            if (observed.next[static_cast<std::size_t>(i)] != last.next[static_cast<std::size_t>(offset + i)]) {
                return false;
            }
        }
        return true;
    };

    // A zero-slide observation must match all visible slots. Checking only
    // the first three slots is exactly what let OJSIO replace OJSIT in the
    // old decoder: the bad fifth slot was then treated as a confirmed queue.
    const bool nextSame = observed.next.size() == last.next.size() &&
                          samePieces(observed.next, last.next);
    if (nextSame && !result.holdChanged) {
        result.plausible = true;
        return result;
    }
    // Prefer the smallest shift when repeated pieces make two alignments
    // possible. This avoids inventing a skipped placement unnecessarily.
    for (int shift = 1; shift <= 2; ++shift) {
        const int required = shift == 1 ? 4 : 3;
        if (overlapMatches(shift, required)) {
            result.slideCount = shift;
            result.nextAdvanced = true;
            result.plausible = !result.holdChanged;
            break;
        }
    }

    // A hold transition and a queue slide can be real, but two simultaneous
    // slides plus a hold change are ambiguous at this sampling rate. Reject
    // that combination instead of guessing an active piece.
    if (result.holdChanged && result.nextAdvanced) {
        result.plausible = result.slideCount == 1;
    } else if (result.holdChanged && nextSame) {
        result.plausible = true;
    }
    if (!result.plausible) return result;

    if (active == Cell::Empty) {
        if (!result.holdChanged && result.nextAdvanced) result.action = "spawn";
        return result;
    }
    if (!result.holdChanged && result.nextAdvanced) {
        result.action = "place";
    } else if (result.holdChanged && result.nextAdvanced && last.hold == Cell::Empty) {
        result.action = observed.hold == last.next[0] ? "first_hold" : "place";
    } else if (result.holdChanged && nextSame) {
        result.action = observed.hold == active ? "hold" : "none";
    } else if (result.holdChanged && result.nextAdvanced) {
        result.action = observed.hold == last.next[0] ? "place_and_hold" : "place";
    }
    return result;
}

QueueObservation intermediateQueueAfterOneSlide(const QueueObservation& before,
                                                const QueueObservation& after);

struct QueuePhase {
    double start = 0;
    double end = 0;
    Cell hold = Cell::Empty;
    std::vector<Cell> next;
    Cell active = Cell::Empty;
    std::string action;
    bool queueManuallyFixed = false;
    // The active piece came from HOLD.  This is separate from `action` because
    // the queue/hold animation can be visible several scans before the lock.
    bool holdUsed = false;
};

constexpr std::size_t QueueWindowSize = 5;
constexpr double SpeculativeSeedPenalty = 25.0;

bool hasFullQueue(const QueueObservation& queue) {
    return queue.next.size() >= QueueWindowSize &&
           std::all_of(queue.next.begin(), queue.next.begin() + QueueWindowSize, [](Cell cell) {
               return isPiece(cell);
           });
}

std::array<Cell, QueueWindowSize> queueWindow(const QueueObservation& queue) {
    std::array<Cell, QueueWindowSize> result{};
    for (std::size_t i = 0; i < QueueWindowSize && i < queue.next.size(); ++i) result[i] = queue.next[i];
    return result;
}

std::vector<Cell> queuePieces(const std::array<Cell, QueueWindowSize>& window) {
    return {window.begin(), window.end()};
}

std::uint8_t pieceMask(Cell piece) {
    if (!isPiece(piece)) return 0;
    return static_cast<std::uint8_t>(1u << (static_cast<unsigned>(piece) - static_cast<unsigned>(Cell::I)));
}

struct QueueRun {
    std::size_t first = 0;
    std::size_t last = 0;
    QueueObservation observation;
    bool manual = false;
};

struct QueueSequenceNode {
    std::array<Cell, QueueWindowSize> window{};
    // Offset of the next generated tail piece in its 7-bag and the known
    // members of that bag.  The initial partial bag only constrains the
    // pieces visible in the video; every later complete bag is exact.
    std::uint8_t nextBagOffset = 0;
    std::uint8_t currentBagMask = 0;
    double score = 0;
    int parent = -1;
    int event = -1;
    int advance = 0;
    int totalAdvance = 0;
    // A seed that differs from the first visible queue is only a hypothesis.
    // It becomes trustworthy after a later scan shows the whole hypothesis
    // exactly; otherwise a redraw glitch must not reinforce it.
    bool speculativeSeed = false;
};

double queueEmissionScore(const std::array<Cell, QueueWindowSize>& candidate,
                          const QueueRun& run) {
    const std::size_t observed = std::min<std::size_t>(QueueWindowSize, run.observation.next.size());
    if (observed == 0) return 0;
    const double runWeight = std::min(3.0, 1.0 + std::log2(static_cast<double>(run.last - run.first + 2)));
    double score = 0;
    for (std::size_t i = 0; i < observed; ++i) {
        if (!isPiece(run.observation.next[i])) continue;
        if (candidate[i] == run.observation.next[i]) score += 3.0 * runWeight;
        else score -= 5.0 * runWeight;
    }
    // A row the reviewer changed is a hard annotation, not weak image
    // evidence. The very large mismatch cost keeps it on the decoded path.
    if (run.manual) {
        for (std::size_t i = 0; i < observed; ++i) {
            if (candidate[i] != run.observation.next[i]) return -1000000.0;
        }
    }
    return score;
}

int queueMismatchCount(const std::array<Cell, QueueWindowSize>& candidate,
                       const QueueRun& run) {
    const std::size_t observed = std::min<std::size_t>(QueueWindowSize, run.observation.next.size());
    int mismatches = 0;
    for (std::size_t i = 0; i < observed; ++i) {
        if (isPiece(run.observation.next[i]) && candidate[i] != run.observation.next[i]) ++mismatches;
    }
    return mismatches;
}

struct HoldRun {
    std::size_t first = 0;
    std::size_t last = 0;
    Cell hold = Cell::Empty;
    std::vector<Cell> next;
    bool manual = false;
};

std::vector<HoldRun> holdRunsFromSamples(const std::vector<QueueRecognitionSample>& samples,
                                         std::size_t first) {
    std::vector<HoldRun> runs;
    for (std::size_t i = first; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        const bool split = runs.empty() || sample.manuallyEdited || runs.back().manual ||
                           sample.observation.hold != runs.back().hold ||
                           !samePieces(sample.observation.next, runs.back().next);
        if (split) {
            runs.push_back({i, i, sample.observation.hold, sample.observation.next,
                            sample.manuallyEdited});
        } else {
            runs.back().last = i;
        }
    }
    return runs;
}

std::vector<std::pair<bool, Cell>> correctHoldBounces(
    const std::vector<QueueRecognitionSample>& samples, std::size_t first) {
    std::vector<std::pair<bool, Cell>> overrides(samples.size(), {false, Cell::Empty});
    const auto runs = holdRunsFromSamples(samples, first);
    for (std::size_t run = 2; run < runs.size(); ++run) {
        const auto& before = runs[run - 2];
        const auto& transient = runs[run - 1];
        const auto& after = runs[run];
        // A real hold swap may change HOLD while NEXT stays still, but when
        // the next piece is subsequently consumed, the swapped-in HOLD must
        // remain visible.  If it returns to the old value exactly at that
        // queue transition, the middle value is a visual false positive.
        if (before.manual || transient.manual || after.manual ||
            before.hold == transient.hold || after.hold != before.hold ||
            !samePieces(before.next, transient.next) ||
            samePieces(transient.next, after.next)) continue;
        for (std::size_t i = transient.first; i <= transient.last; ++i) {
            overrides[i] = {true, before.hold};
        }
    }
    return overrides;
}

bool initializeSevenBag(const std::array<Cell, QueueWindowSize>& window, int phase,
                        bool useSevenBag, std::uint8_t& nextOffset, std::uint8_t& mask) {
    nextOffset = 0;
    mask = 0;
    if (!useSevenBag) return true;
    for (std::size_t i = 0; i < window.size(); ++i) {
        const int offset = (phase + static_cast<int>(i)) % 7;
        if (offset == 0) mask = 0;
        const std::uint8_t bit = pieceMask(window[i]);
        if (bit == 0 || (mask & bit) != 0) return false;
        mask = static_cast<std::uint8_t>(mask | bit);
    }
    nextOffset = static_cast<std::uint8_t>((phase + static_cast<int>(window.size())) % 7);
    if (nextOffset == 0) mask = 0;
    return true;
}

bool appendQueuePiece(QueueSequenceNode& node, Cell piece, bool useSevenBag) {
    if (!isPiece(piece)) return false;
    if (useSevenBag) {
        if (node.nextBagOffset == 0) node.currentBagMask = 0;
        const std::uint8_t bit = pieceMask(piece);
        if ((node.currentBagMask & bit) != 0) return false;
        node.currentBagMask = static_cast<std::uint8_t>(node.currentBagMask | bit);
        node.nextBagOffset = static_cast<std::uint8_t>((node.nextBagOffset + 1) % 7);
        if (node.nextBagOffset == 0) node.currentBagMask = 0;
    }
    std::move(node.window.begin() + 1, node.window.end(), node.window.begin());
    node.window.back() = piece;
    return true;
}

bool sameQueueNodeState(const QueueSequenceNode& a, const QueueSequenceNode& b) {
    return a.window == b.window && a.nextBagOffset == b.nextBagOffset &&
           a.currentBagMask == b.currentBagMask && a.speculativeSeed == b.speculativeSeed;
}

bool betterQueueNode(const QueueSequenceNode& a, const QueueSequenceNode& b) {
    if (std::abs(a.score - b.score) > .000001) return a.score > b.score;
    if (a.totalAdvance != b.totalAdvance) return a.totalAdvance < b.totalAdvance;
    return a.advance < b.advance;
}

void pruneQueueBeam(std::vector<QueueSequenceNode>& candidates, int width) {
    std::sort(candidates.begin(), candidates.end(), betterQueueNode);
    std::vector<QueueSequenceNode> unique;
    unique.reserve(std::min<std::size_t>(candidates.size(), static_cast<std::size_t>(width)));
    for (const QueueSequenceNode& candidate : candidates) {
        const bool alreadyPresent = std::any_of(unique.begin(), unique.end(), [&](const QueueSequenceNode& kept) {
            return sameQueueNodeState(candidate, kept);
        });
        if (alreadyPresent) continue;
        unique.push_back(candidate);
        if (unique.size() >= static_cast<std::size_t>(width)) break;
    }
    candidates = std::move(unique);
}

std::vector<QueueRun> queueRunsFromSamples(const std::vector<QueueRecognitionSample>& samples,
                                            std::size_t first) {
    std::vector<QueueRun> runs;
    for (std::size_t i = first; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        const bool split = runs.empty() || sample.manuallyEdited || runs.back().manual ||
                           !samePieces(sample.observation.next, runs.back().observation.next);
        if (split) {
            runs.push_back({i, i, sample.observation, sample.manuallyEdited});
        } else {
            runs.back().last = i;
        }
    }
    return runs;
}

void decodeQueueSequence(std::vector<QueueRecognitionSample>& samples, const Settings& settings) {
    for (auto& sample : samples) {
        sample.decoded = {};
        sample.sequenceCorrected = false;
        sample.holdCorrected = false;
        sample.rejected = false;
    }
    const auto first = std::find_if(samples.begin(), samples.end(), [](const QueueRecognitionSample& sample) {
        return hasFullQueue(sample.observation);
    });
    if (first == samples.end()) return;
    const std::size_t firstIndex = static_cast<std::size_t>(std::distance(samples.begin(), first));
    const auto runs = queueRunsFromSamples(samples, firstIndex);
    if (runs.empty()) return;
    const auto holdOverrides = correctHoldBounces(samples, firstIndex);

    std::vector<QueueSequenceNode> seedCandidates;
    const auto initial = queueWindow(runs.front().observation);
    std::array<Cell, QueueWindowSize> candidate = initial;
    const int correctionBudget = runs.front().manual ? 0 : 1;
    const auto generateSeeds = [&](auto&& self, std::size_t slot, int edits) -> void {
        if (slot == candidate.size()) {
            for (int phase = 0; phase < (settings.queueUseSevenBag ? 7 : 1); ++phase) {
                QueueSequenceNode node;
                node.window = candidate;
                if (!initializeSevenBag(node.window, phase, settings.queueUseSevenBag,
                                        node.nextBagOffset, node.currentBagMask)) continue;
                // Do not let a one-slot correction of the first trusted
                // window become a second, self-reinforcing history.  Without
                // this cost, a later redraw glitch can make that speculative
                // seed outscore the exact first observation and permanently
                // move the whole beam onto a different queue.
                node.score = queueEmissionScore(node.window, runs.front()) - edits * SpeculativeSeedPenalty;
                node.event = 0;
                node.speculativeSeed = edits > 0;
                seedCandidates.push_back(node);
            }
            return;
        }
        const Cell observed = initial[slot];
        candidate[slot] = observed;
        self(self, slot + 1, edits);
        if (edits >= correctionBudget) return;
        for (Cell piece : Pieces) {
            if (piece == observed) continue;
            candidate[slot] = piece;
            self(self, slot + 1, edits + 1);
        }
        candidate[slot] = observed;
    };
    generateSeeds(generateSeeds, 0, 0);
    pruneQueueBeam(seedCandidates, settings.queueBeamWidth);
    if (seedCandidates.empty()) return;

    std::vector<QueueSequenceNode> nodes;
    nodes.reserve(runs.size() * static_cast<std::size_t>(settings.queueBeamWidth));
    std::vector<int> beam;
    beam.reserve(seedCandidates.size());
    for (const auto& seed : seedCandidates) {
        beam.push_back(static_cast<int>(nodes.size()));
        nodes.push_back(seed);
    }

    for (std::size_t event = 1; event < runs.size(); ++event) {
        std::vector<QueueSequenceNode> candidates;
        candidates.reserve(beam.size() * 16);
        for (const int parentIndex : beam) {
            const QueueSequenceNode& parent = nodes[static_cast<std::size_t>(parentIndex)];
            for (int advance = 0; advance <= 2; ++advance) {
                std::vector<QueueSequenceNode> frontier{parent};
                for (int shift = 0; shift < advance; ++shift) {
                    std::vector<QueueSequenceNode> appended;
                    appended.reserve(frontier.size() * Pieces.size());
                    for (const auto& state : frontier) {
                        for (Cell piece : Pieces) {
                            QueueSequenceNode next = state;
                            if (appendQueuePiece(next, piece, settings.queueUseSevenBag)) appended.push_back(next);
                        }
                    }
                    frontier = std::move(appended);
                    if (frontier.empty()) break;
                }
                for (auto& state : frontier) {
                    // A bag correction may repair a single bad colour class,
                    // but it must not replace most of a visible queue with a
                    // hypothetical 7-bag continuation.  That is how a short
                    // visual effect such as SIIIO used to turn the stable
                    // SJTZL queue into an unrelated TZLIO path.
                    const int mismatchLimit = runs[event].manual ? 0 : 1;
                    if (state.speculativeSeed &&
                        !samePieces(queuePieces(state.window), runs[event].observation.next)) continue;
                    if (queueMismatchCount(state.window, runs[event]) > mismatchLimit) continue;
                    state.parent = parentIndex;
                    state.event = static_cast<int>(event);
                    state.advance = advance;
                    state.totalAdvance = parent.totalAdvance + advance;
                    // A real advance is slightly less likely than staying on
                    // the same queue, but image evidence dominates whenever
                    // four/five overlapping slots support a slide.
                    state.score = parent.score + queueEmissionScore(state.window, runs[event])
                                  - (advance == 1 ? .75 : advance == 2 ? 2.25 : 0.0);
                    if (state.speculativeSeed &&
                        samePieces(queuePieces(state.window), runs[event].observation.next)) {
                        state.speculativeSeed = false;
                    }
                    candidates.push_back(std::move(state));
                }
            }
            if (!runs[event].manual) {
                // Keep the last trusted queue when this scan cannot be
                // explained by a legal 0/1/2-slide transition with at most
                // one visual correction. A later clean scan can still resume
                // the normal transition path.
                QueueSequenceNode fallback = parent;
                fallback.parent = parentIndex;
                fallback.event = static_cast<int>(event);
                fallback.advance = 0;
                fallback.totalAdvance = parent.totalAdvance;
                fallback.score = parent.score + queueEmissionScore(parent.window, runs[event]) - 20.0;
                candidates.push_back(std::move(fallback));
            }
        }
        pruneQueueBeam(candidates, settings.queueBeamWidth);
        if (candidates.empty()) break;
        beam.clear();
        beam.reserve(candidates.size());
        for (const auto& state : candidates) {
            beam.push_back(static_cast<int>(nodes.size()));
            nodes.push_back(state);
        }
    }
    if (beam.empty()) return;

    std::vector<std::array<Cell, QueueWindowSize>> decoded(runs.size());
    int nodeIndex = beam.front();
    while (nodeIndex >= 0) {
        const auto& node = nodes[static_cast<std::size_t>(nodeIndex)];
        if (node.event >= 0 && static_cast<std::size_t>(node.event) < decoded.size()) decoded[static_cast<std::size_t>(node.event)] = node.window;
        nodeIndex = node.parent;
    }
    for (std::size_t event = 0; event < runs.size(); ++event) {
        const auto correctedNext = queuePieces(decoded[event]);
        for (std::size_t i = runs[event].first; i <= runs[event].last; ++i) {
            auto& sample = samples[i];
            sample.decoded = sample.observation;
            if (i < holdOverrides.size() && holdOverrides[i].first) {
                sample.decoded.hold = holdOverrides[i].second;
                sample.holdCorrected = true;
            }
            sample.decoded.next = correctedNext;
            sample.sequenceCorrected = !samePieces(sample.observation.next, correctedNext);
            const int mismatchLimit = runs[event].manual ? 0 : 1;
            sample.rejected = !validQueue(sample.observation) ||
                              queueMismatchCount(decoded[event], runs[event]) > mismatchLimit;
        }
    }
}

class QueuePhaseBuilder {
public:
    explicit QueuePhaseBuilder(int confirmSamples) : confirmSamples_(confirmSamples) {}

    void push(double time, const QueueObservation& observed) {
        const bool wasFirstDetected = firstDetected_;
        QueueTransition possible;
        if (wasFirstDetected) {
            possible = checkQueueTransition(observed, lastStable_, active_);
            const bool isStableQueue = sameQueue(observed, lastStable_) ||
                                       (possible.plausible && possible.action == "none" && possible.holdChanged == false);
            if (!isStableQueue && !possible.plausible) {
                // Keep the raw row for later review, but do not let an
                // impossible recognition enter the confirmation counter.
                QueueRecognitionSample rejected{time, active_, observed, false, false};
                rejected.rejected = true;
                rawSamples_.push_back(std::move(rejected));
                return;
            }
        }
        if (sameQueue(observed, pending_)) {
            ++pendingCount_;
        } else {
            pending_ = observed;
            pendingCount_ = 1;
        }
        rawSamples_.push_back({time, active_, observed, pendingCount_ >= confirmSamples_, false});
        if (pendingCount_ < confirmSamples_) return;

        if (!firstDetected_ && validQueue(observed)) {
            lastStable_ = observed;
            active_ = Cell::Empty;
            phases_.push_back({0, time, Cell::Empty, observed.next, Cell::Empty, "init", false});
            currentStart_ = time;
            activeFromHold_ = false;
            firstDetected_ = true;
            return;
        }
        if (!firstDetected_) return;

        const QueueTransition transition = checkQueueTransition(observed, lastStable_, active_);
        if (transition.action == "spawn" || transition.action == "first_hold") {
            if (transition.slideCount >= 2 && lastStable_.next.size() >= 2) {
                const QueueObservation intermediate = intermediateQueueAfterOneSlide(lastStable_, observed);
                phases_.push_back({time, time, lastStable_.hold, intermediate.next,
                                   lastStable_.next[0], "spawn_skipped", false});
                active_ = lastStable_.next[1];
                activeFromHold_ = false;
            } else if (transition.action == "first_hold") {
                // The current piece went into an empty HOLD slot.  The queue
                // has advanced, so the newly controlled piece is now the
                // first item of the observed queue.
                active_ = observed.next.empty() ? Cell::Empty : observed.next[0];
                activeFromHold_ = true;
            } else if (activeFromHold_) {
                // Some videos show the HOLD change one scan before the queue
                // advances.  In that case lastStable_.next[0] is still the
                // newly spawned piece; keep the pending HOLD provenance when
                // it becomes active.
                active_ = lastStable_.next.empty() ? Cell::Empty : lastStable_.next[0];
            } else {
                active_ = lastStable_.next[0];
                activeFromHold_ = false;
            }
        } else if (transition.action == "hold") {
            // An empty HOLD slot does not provide a replacement active piece
            // yet.  Keep the hold provenance pending until the delayed queue
            // slide exposes the next active piece.
            active_ = lastStable_.hold == Cell::Empty ? Cell::Empty : lastStable_.hold;
            activeFromHold_ = true;
        } else if (transition.action == "place" || transition.action == "place_and_hold") {
            phases_.push_back({currentStart_, time, lastStable_.hold, lastStable_.next, active_, transition.action,
                               false, activeFromHold_ || transition.action == "place_and_hold"});
            currentStart_ = time;
            if (transition.slideCount >= 2 && lastStable_.next.size() >= 2) {
                const QueueObservation intermediate = intermediateQueueAfterOneSlide(lastStable_, observed);
                phases_.push_back({time, time, lastStable_.hold, intermediate.next,
                                   lastStable_.next[0], "place_skipped", false});
                active_ = lastStable_.next[1];
                activeFromHold_ = false;
            } else if (transition.action == "place_and_hold") {
                // The queue head was moved into HOLD while the old active
                // piece was locked.  The old HOLD piece is now active.
                active_ = lastStable_.hold;
                activeFromHold_ = true;
            } else {
                active_ = lastStable_.next[0];
                activeFromHold_ = false;
            }
        }
        // This update intentionally happens even when a changed hold/next
        // produced action=none. That is how the original code suppresses
        // display-effect glitches without leaving the stale queue forever.
        if (transition.plausible && (transition.holdChanged || transition.nextAdvanced)) lastStable_ = observed;
        // The active mino may change while processing a stable transition, so
        // store the post-transition internal state on this exact scan row.
        rawSamples_.back().active = active_;
    }

    std::vector<QueuePhase> finish(double duration) const {
        auto phases = phases_;
        if (currentStart_ < duration) {
            phases.push_back({currentStart_, duration, lastStable_.hold, lastStable_.next, active_, "end", false,
                              activeFromHold_});
        }
        for (auto& phase : phases) {
            phase.queueManuallyFixed = std::any_of(rawSamples_.begin(), rawSamples_.end(), [&](const QueueRecognitionSample& sample) {
                return sample.manuallyEdited && sample.timeSeconds >= phase.start - .000001 &&
                       sample.timeSeconds <= phase.end + .000001;
            });
        }
        return phases;
    }

    const std::vector<QueueRecognitionSample>& rawSamples() const { return rawSamples_; }

private:
    int confirmSamples_ = 2;
    QueueObservation pending_;
    int pendingCount_ = 0;
    QueueObservation lastStable_;
    Cell active_ = Cell::Empty;
    bool activeFromHold_ = false;
    double currentStart_ = 0;
    bool firstDetected_ = false;
    std::vector<QueuePhase> phases_;
    std::vector<QueueRecognitionSample> rawSamples_;
};

std::vector<QueuePhase> buildQueuePhasesFromSamples(std::vector<QueueRecognitionSample>& samples,
                                                    const Settings& settings, double duration) {
    decodeQueueSequence(samples, settings);
    QueuePhaseBuilder builder(settings.queueConfirmSamples);
    for (const auto& sample : samples) {
        const QueueObservation& queue = sample.decoded.next.empty() ? sample.observation : sample.decoded;
        builder.push(sample.timeSeconds, queue);
    }
    const auto& decodedSamples = builder.rawSamples();
    for (std::size_t i = 0; i < samples.size() && i < decodedSamples.size(); ++i) {
        // A reanalysis rebuilds the inferred fields, but an explicit editor
        // choice is ground truth for that scan row.  Do not erase a manually
        // corrected active mino while refreshing the transition decoder.
        if (!samples[i].manuallyEdited || !isPiece(samples[i].active)) {
            samples[i].active = decodedSamples[i].active;
        }
        if (!samples[i].manuallyEdited) {
            samples[i].stable = decodedSamples[i].stable;
            // Keep a bad raw visual observation visible even if the phase
            // builder itself can continue through the repaired queue.
            samples[i].rejected = samples[i].rejected || decodedSamples[i].rejected;
        } else {
            samples[i].stable = true;
            samples[i].rejected = false;
        }
    }
    auto phases = builder.finish(duration);
    for (auto& phase : phases) {
        // A phase is closed by placing the active mino immediately before its
        // end.  Its first raw sample can still describe the previous piece
        // during a hold/queue-slide animation (the opening T->Hold, Z spawn
        // is a concrete example), so choose the latest pre-boundary sample.
        // Prefer an explicit editor choice over inferred evidence.
        const QueueRecognitionSample* activeSample = nullptr;
        for (const auto& sample : samples) {
            if (sample.timeSeconds < phase.start - .000001 || sample.timeSeconds >= phase.end - .000001) continue;
            if (!isPiece(sample.active)) continue;
            if (!activeSample || (sample.manuallyEdited && !activeSample->manuallyEdited) ||
                (sample.manuallyEdited == activeSample->manuallyEdited && sample.timeSeconds > activeSample->timeSeconds)) {
                activeSample = &sample;
            }
        }
        if (activeSample) phase.active = activeSample->active;
        phase.queueManuallyFixed = std::any_of(samples.begin(), samples.end(), [&](const QueueRecognitionSample& sample) {
            return sample.manuallyEdited && sample.timeSeconds >= phase.start - .000001 &&
                   sample.timeSeconds <= phase.end + .000001;
        });
    }
    return phases;
}

Board cleanUpBoard(const VisibleBoard& visible) {
    Board board = VisionAnalyzer::makeFullBoard(visible);
    int firstNonGarbageRow = -1;
    for (int y = BoardHeight - 1; y >= 0; --y) {
        bool includesGarbage = false;
        for (int x = 0; x < BoardWidth; ++x) {
            if (board[index(x, y)] == Cell::Garbage) { includesGarbage = true; break; }
        }
        if (!includesGarbage) { firstNonGarbageRow = y; break; }
    }
    if (firstNonGarbageRow != -1) {
        for (int y = firstNonGarbageRow - 1; y >= 0; --y) {
            for (int x = 0; x < BoardWidth; ++x) {
                if (board[index(x, y)] == Cell::Garbage) board[index(x, y)] = Cell::Empty;
            }
        }
    }
    return board;
}

struct BoardRequest {
    double time = 0;
    int phase = 0;
};

std::vector<BoardRequest> makeBoardRequests(const std::vector<QueuePhase>& phases, int onnxSamples) {
    std::vector<BoardRequest> result;
    for (int i = 0; i < static_cast<int>(phases.size()); ++i) {
        const double duration = phases[i].end - phases[i].start;
        const int samples = duration < .1 ? 1 : onnxSamples;
        for (int s = 1; s <= samples; ++s) {
            result.push_back({phases[i].start + duration * s / (samples + 1.0), i});
        }
    }
    return result;
}

bool collectQueuePhases(const std::filesystem::path& input, const Settings& settings, Status& status,
                        std::vector<QueuePhase>& p1, std::vector<QueuePhase>& p2,
                        std::vector<QueueRecognitionSample>& p1Samples,
                        std::vector<QueueRecognitionSample>& p2Samples,
                        double& durationOut, std::string& error) {
    VideoReader reader;
    if (!reader.open(input, error)) return false;
    VisionAnalyzer p1Vision(reader.width(), reader.height(), settings.p1, nullptr);
    VisionAnalyzer p2Vision(reader.width(), reader.height(), settings.p2, nullptr);
    const double duration = reader.durationSeconds();
    durationOut = duration;
    double nextScan = 0;
    Frame frame;
    Frame lastFrame;
    bool haveFrame = false;
    bool eos = false;
    std::uint64_t scans = 0;

    const auto scanFrame = [&](const Frame& source, double time) {
        if (settings.player1Enabled) p1Samples.push_back({time, Cell::Empty, p1Vision.observeQueue(source)});
        if (settings.player2Enabled) p2Samples.push_back({time, Cell::Empty, p2Vision.observeQueue(source)});
        ++scans;
    };

    while (!eos) {
        if (status.cancel.load()) { error = "Recovery cancelled"; return false; }
        if (!reader.read(frame, eos, error)) return false;
        if (eos || frame.bgra.empty()) continue;
        lastFrame = std::move(frame);
        haveFrame = true;
        const double frameTime = static_cast<double>(lastFrame.time100ns) / 10000000.0;
        // Original code scans at every 0.01 s seek point. With sequential
        // decoding we reuse the current decoded frame for all such points
        // until the next decoded frame arrives; this preserves its
        // two-identical-scan confirmation behavior while avoiding seeks.
        while (nextScan <= frameTime + .000001 && (duration <= 0 || nextScan < duration)) {
            scanFrame(lastFrame, nextScan);
            nextScan += settings.sampleIntervalSeconds;
        }
        const int pct = duration > 0 ? static_cast<int>(std::clamp(frameTime / duration * 35.0, 0.0, 35.0)) : 0;
        status.progress.store(pct);
        status.setMessage("Pass 1/3: queue scan " + std::to_string(scans));
    }
    if (!haveFrame) { error = "No decodable video frame was found"; return false; }
    while (duration > 0 && nextScan < duration) {
        scanFrame(lastFrame, nextScan);
        nextScan += settings.sampleIntervalSeconds;
    }
    status.setMessage("Pass 1/3: global 7-bag NEXT reconstruction");
    p1 = settings.player1Enabled ? buildQueuePhasesFromSamples(p1Samples, settings, duration)
                                 : std::vector<QueuePhase>{};
    p2 = settings.player2Enabled ? buildQueuePhasesFromSamples(p2Samples, settings, duration)
                                 : std::vector<QueuePhase>{};
    return true;
}

bool collectBoardSamples(const std::filesystem::path& input, const std::filesystem::path& modelPath,
                         const Settings& settings, const std::vector<QueuePhase>& p1Phases,
                         const std::vector<QueuePhase>& p2Phases, Status& status,
                         std::vector<std::vector<BoardObservation>>& p1Samples,
                         std::vector<std::vector<BoardObservation>>& p2Samples, std::string& error) {
    VideoReader reader;
    if (!reader.open(input, error)) return false;

    std::unique_ptr<OnnxBoardModel> p1Model;
    std::unique_ptr<OnnxBoardModel> p2Model;
    if (settings.player1Enabled) {
        p1Model = std::make_unique<OnnxBoardModel>(modelPath, error);
        if (!p1Model->ready()) return false;
    }
    if (settings.player2Enabled) {
        p2Model = std::make_unique<OnnxBoardModel>(modelPath, error);
        if (!p2Model->ready()) return false;
    }
    VisionAnalyzer p1Vision(reader.width(), reader.height(), settings.p1, p1Model.get());
    VisionAnalyzer p2Vision(reader.width(), reader.height(), settings.p2, p2Model.get());
    const auto p1Requests = makeBoardRequests(p1Phases, settings.onnxSamples);
    const auto p2Requests = makeBoardRequests(p2Phases, settings.onnxSamples);
    p1Samples.assign(p1Phases.size(), {});
    p2Samples.assign(p2Phases.size(), {});
    std::size_t p1Index = 0;
    std::size_t p2Index = 0;
    const std::size_t totalRequests = p1Requests.size() + p2Requests.size();
    std::size_t completeRequests = 0;
    Frame frame;
    Frame lastFrame;
    bool haveFrame = false;
    bool eos = false;

    const auto process = [&](const Frame& source, const BoardRequest& request, VisionAnalyzer& vision,
                             std::vector<std::vector<BoardObservation>>& samples) -> bool {
        auto observation = vision.analyzeBoard(source);
        if (!observation.recognitionError.empty()) {
            error = observation.recognitionError;
            return false;
        }
        observation.timeSeconds = request.time;
        samples[request.phase].push_back(std::move(observation));
        return true;
    };

    while (!eos) {
        if (status.cancel.load()) { error = "Recovery cancelled"; return false; }
        if (!reader.read(frame, eos, error)) return false;
        if (eos || frame.bgra.empty()) continue;
        lastFrame = std::move(frame);
        haveFrame = true;
        const double frameTime = static_cast<double>(lastFrame.time100ns) / 10000000.0;
        while (p1Index < p1Requests.size() && p1Requests[p1Index].time <= frameTime + .000001) {
            if (!process(lastFrame, p1Requests[p1Index], p1Vision, p1Samples)) return false;
            ++p1Index;
            ++completeRequests;
        }
        while (p2Index < p2Requests.size() && p2Requests[p2Index].time <= frameTime + .000001) {
            if (!process(lastFrame, p2Requests[p2Index], p2Vision, p2Samples)) return false;
            ++p2Index;
            ++completeRequests;
        }
        const int pct = totalRequests > 0
            ? 35 + static_cast<int>(completeRequests * 45 / totalRequests)
            : 80;
        status.progress.store(std::min(pct, 80));
        status.setMessage("Pass 2/3: ONNX board inference " + std::to_string(completeRequests) + "/" + std::to_string(totalRequests));
    }
    if (!haveFrame) { error = "No decodable video frame was found"; return false; }
    while (p1Index < p1Requests.size()) {
        if (!process(lastFrame, p1Requests[p1Index], p1Vision, p1Samples)) return false;
        ++p1Index;
    }
    while (p2Index < p2Requests.size()) {
        if (!process(lastFrame, p2Requests[p2Index], p2Vision, p2Samples)) return false;
        ++p2Index;
    }
    return true;
}

std::vector<TimelineStep> buildRawTimeline(const std::vector<QueuePhase>& phases,
                                           const std::vector<std::vector<BoardObservation>>& samples) {
    std::vector<TimelineStep> result;
    result.reserve(phases.size());
    for (std::size_t i = 0; i < phases.size(); ++i) {
        const BoardObservation voted = i < samples.size() ? VisionAnalyzer::aggregate(samples[i]) : BoardObservation{};
        TimelineStep step;
        step.startSeconds = phases[i].start;
        step.timeSeconds = phases[i].end;
        step.piece = phases[i].active;
        step.action = phases[i].action;
        step.hold = phases[i].hold;
        step.next = phases[i].next;
        step.holdUsed = phases[i].holdUsed;
        step.queueManuallyFixed = phases[i].queueManuallyFixed;
        step.confidence = voted.confidence;
        step.observed = cleanUpBoard(voted.board);
        step.board = step.observed;
        step.fullBoard = step.observed;
        result.push_back(std::move(step));
    }
    return result;
}

QueueObservation intermediateQueueAfterOneSlide(const QueueObservation& before,
                                                const QueueObservation& after) {
    QueueObservation result = before;
    result.next.clear();
    for (std::size_t i = 1; i < before.next.size(); ++i) result.next.push_back(before.next[i]);
    if (!after.next.empty()) result.next.push_back(after.next.front());
    if (result.next.size() > after.next.size()) result.next.resize(after.next.size());
    return result;
}

std::vector<TimelineStep> buildRawTimelineFromFlatBoards(const std::vector<QueuePhase>& phases,
                                                         const std::vector<BoardObservation>& flatSamples) {
    std::vector<std::vector<BoardObservation>> grouped(phases.size());
    for (const auto& sample : flatSamples) {
        for (std::size_t phase = 0; phase < phases.size(); ++phase) {
            const bool last = phase + 1 == phases.size();
            if (sample.timeSeconds >= phases[phase].start - .000001 &&
                (sample.timeSeconds < phases[phase].end - .000001 || last)) {
                grouped[phase].push_back(sample);
                break;
            }
        }
    }
    // Queue edits can move a phase boundary away from the original ONNX
    // request slots. Reuse the nearest retained board observation rather than
    // silently replacing that phase with an all-empty board; the legal-move
    // beam can then still evaluate the corrected queue immediately.
    if (!flatSamples.empty()) {
        for (std::size_t phase = 0; phase < phases.size(); ++phase) {
            if (!grouped[phase].empty()) continue;
            const double target = (phases[phase].start + phases[phase].end) * .5;
            const auto nearest = std::min_element(flatSamples.begin(), flatSamples.end(),
                [target](const BoardObservation& left, const BoardObservation& right) {
                    return std::abs(left.timeSeconds - target) < std::abs(right.timeSeconds - target);
                });
            grouped[phase].push_back(*nearest);
        }
    }
    return buildRawTimeline(phases, grouped);
}

struct PageEntry {
    double time = 0;
    std::string board;
    std::string activePiece;
    std::string hold;
    std::string next;
    std::string operationPiece;
    int operationX = 0;
    int operationY = 0;
    int operationRotation = 0;
    bool operationHold = false;
};

std::string operationRotationName(int rotation) {
    switch (rotation & 3) {
    case 1: return "right";
    case 2: return "reverse";
    case 3: return "left";
    default: return "spawn";
    }
}

std::string boardString(const Board& board) {
    std::string result;
    result.reserve(board.size());
    for (Cell cell : board) result.push_back(cellChar(cell));
    return result;
}

std::string sanitizePieces(const std::vector<Cell>& pieces) { return pieceString(pieces); }
std::string sanitizePiece(Cell piece) { return isPiece(piece) ? std::string(1, cellChar(piece)) : std::string(); }

void writeQueueRecognitionJson(std::ostream& out, const std::vector<QueueRecognitionSample>& samples) {
    out << '[';
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i) out << ',';
        const auto& sample = samples[i];
        out << "{\"time\":" << std::fixed << std::setprecision(3) << sample.timeSeconds
            << ",\"active\":\"" << sanitizePiece(sample.active)
            << "\",\"hold\":\"" << sanitizePiece(sample.observation.hold)
            << "\",\"next\":\"" << sanitizePieces(sample.observation.next)
            << "\",\"decodedHold\":\"" << sanitizePiece(sample.decoded.hold)
            << "\",\"decodedNext\":\"" << sanitizePieces(sample.decoded.next)
            << "\",\"stable\":" << (sample.stable ? "true" : "false")
            << ",\"manual\":" << (sample.manuallyEdited ? "true" : "false")
            << ",\"sequenceCorrected\":" << (sample.sequenceCorrected ? "true" : "false")
            << ",\"holdCorrected\":" << (sample.holdCorrected ? "true" : "false")
            << ",\"rejected\":" << (sample.rejected ? "true" : "false") << '}';
    }
    out << ']';
}

std::string simulatorQueueForStep(const TimelineStep& step) {
    // `next` is the visible queue after the active mino was spawned.  The
    // Simulator has no separate active-mino field, so put it back at the
    // queue head.  This makes every exported page chronological: the mino
    // about to be controlled is always the first visible item.
    std::string queue = sanitizePiece(step.piece) + sanitizePieces(step.next);
    return queue;
}

std::vector<PageEntry> pageEntries(const std::vector<TimelineStep>& timeline) {
    // A TimelineStep is a state while `piece` is being controlled. Its board
    // becomes the result of placing the previous step's active piece. The
    // original HTML flattened every state at `timeSeconds` and copied only
    // `next`; that made the active mino appear from nowhere in the viewer.
    // Serialize phase starts instead, and retain the previous queue during a
    // line-clear landing frame.
    std::vector<PageEntry> entries;
    double lastTime = -1;
    const auto append = [&](double requestedTime, const Board& board, const TimelineStep& queueStep,
                            const TimelineStep* operationStep) {
        double time = requestedTime;
        if (time <= lastTime) time = lastTime + .01;
        PageEntry entry;
        entry.time = time;
        entry.board = boardString(board);
        entry.activePiece = sanitizePiece(queueStep.piece);
        entry.hold = sanitizePiece(queueStep.hold);
        entry.next = simulatorQueueForStep(queueStep);
        if (operationStep && isPiece(operationStep->placedPiece)) {
            entry.operationPiece = sanitizePiece(operationStep->placedPiece);
            entry.operationX = operationStep->placementX;
            entry.operationY = operationStep->placementY;
            entry.operationRotation = operationStep->placementRotation;
            entry.operationHold = queueStep.holdUsed;
        }
        entries.push_back(std::move(entry));
        lastTime = time;
    };
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        const auto& step = timeline[i];
        // Timeline[i].board is the field before the current active piece is
        // locked. The solver stores that exact lock on timeline[i + 1], so
        // export the page as (field, operation) without a board-diff guess.
        const TimelineStep* operationStep = i + 1 < timeline.size() ? &timeline[i + 1] : nullptr;
        append(step.startSeconds, step.board, step, operationStep);
    }
    return entries;
}

std::vector<std::pair<char, int>> encodeRle(const std::string& data) {
    std::vector<std::pair<char, int>> result;
    if (data.empty()) return result;
    char previous = data[0];
    int count = 1;
    for (std::size_t i = 1; i < data.size(); ++i) {
        if (data[i] == previous) ++count;
        else { result.push_back({previous, count}); previous = data[i]; count = 1; }
    }
    result.push_back({previous, count});
    return result;
}

std::string difference(const std::string& previous, const std::string& current) {
    std::string result(current.size(), 'E');
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (previous[i] != current[i]) result[i] = current[i];
    }
    return result;
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    for (char c : value) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string jsonRle(const std::string& data) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto [value, count] : encodeRle(data)) {
        if (!first) out << ',';
        first = false;
        out << "[\"" << value << "\"," << count << ']';
    }
    out << ']';
    return out.str();
}

std::string base64(const std::string& text) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((text.size() + 2) / 3 * 4);
    std::uint32_t value = 0;
    int bits = -6;
    for (unsigned char c : text) {
        value = (value << 8) | c;
        bits += 8;
        while (bits >= 0) { result.push_back(table[(value >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) result.push_back(table[((value << 8) >> (bits + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');
    return result;
}

std::string singlePagesJson(const std::vector<PageEntry>& pages) {
    std::ostringstream out;
    out << '[';
    std::string previous;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (i) out << ',';
        const auto& page = pages[i];
        const std::string encoded = previous.empty() ? page.board : difference(previous, page.board);
        out << "{\"p1\":{\"b\":" << jsonRle(encoded)
            << ",\"h\":\"" << jsonEscape(page.hold)
            << "\",\"n\":\"" << jsonEscape(page.next) << "\"";
        if (!page.operationPiece.empty()) {
            out << ",\"o\":{\"type\":\"" << page.operationPiece
                << "\",\"rotation\":\"" << operationRotationName(page.operationRotation)
                << "\",\"x\":" << page.operationX
                << ",\"y\":" << page.operationY;
            out << ",\"coordinateSpace\":\"simulator\"";
            if (page.operationHold) out << ",\"hold\":true";
            out << '}';
        }
        out << "}}";
        previous = page.board;
    }
    out << ']';
    return out.str();
}

std::string combinedPagesJson(const std::vector<PageEntry>& p1, const std::vector<PageEntry>& p2) {
    std::vector<double> times;
    times.reserve(p1.size() + p2.size());
    for (const auto& page : p1) times.push_back(page.time);
    for (const auto& page : p2) times.push_back(page.time);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    std::size_t p1Index = 0;
    std::size_t p2Index = 0;
    std::string previous1;
    std::string previous2;
    std::string lastState;
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const double time : times) {
        while (p1Index + 1 < p1.size() && p1[p1Index + 1].time <= time) ++p1Index;
        while (p2Index + 1 < p2.size() && p2[p2Index + 1].time <= time) ++p2Index;
        if (p1Index >= p1.size() || p2Index >= p2.size()) continue;
        const auto& a = p1[p1Index];
        const auto& b = p2[p2Index];
        const auto operationState = [](const PageEntry& page) {
            return page.operationPiece + ':' + std::to_string(page.operationX) + ':'
                + std::to_string(page.operationY) + ':' + std::to_string(page.operationRotation)
                + ':' + (page.operationHold ? "1" : "0");
        };
        const std::string state = a.board + '\x1F' + b.board + '\x1F' + a.hold + '\x1F' + a.next + '\x1F' + operationState(a)
            + '\x1F' + b.hold + '\x1F' + b.next + '\x1F' + operationState(b);
        if (state == lastState) continue;
        lastState = state;
        if (!first) out << ',';
        first = false;
        const std::string diff1 = previous1.empty() ? a.board : difference(previous1, a.board);
        const std::string diff2 = previous2.empty() ? b.board : difference(previous2, b.board);
        out << "{\"p1\":{\"b\":" << jsonRle(diff1)
            << ",\"h\":\"" << jsonEscape(a.hold) << "\",\"n\":\"" << jsonEscape(a.next) << "\"";
        if (!a.operationPiece.empty()) {
            out << ",\"o\":{\"type\":\"" << a.operationPiece
                << "\",\"rotation\":\"" << operationRotationName(a.operationRotation)
                << "\",\"x\":" << a.operationX << ",\"y\":" << a.operationY;
            out << ",\"coordinateSpace\":\"simulator\"";
            if (a.operationHold) out << ",\"hold\":true";
            out << '}';
        }
        out << "}"
            << ",\"p2\":{\"b\":" << jsonRle(diff2)
            << ",\"h\":\"" << jsonEscape(b.hold) << "\",\"n\":\"" << jsonEscape(b.next) << "\"";
        if (!b.operationPiece.empty()) {
            out << ",\"o\":{\"type\":\"" << b.operationPiece
                << "\",\"rotation\":\"" << operationRotationName(b.operationRotation)
                << "\",\"x\":" << b.operationX << ",\"y\":" << b.operationY;
            out << ",\"coordinateSpace\":\"simulator\"";
            if (b.operationHold) out << ",\"hold\":true";
            out << '}';
        }
        out << "}}";
        previous1 = a.board;
        previous2 = b.board;
    }
    out << ']';
    return out.str();
}

std::string boardMatrixJson(const std::string& board) {
    std::ostringstream out;
    out << '[';
    for (int y = 0; y < BoardHeight; ++y) {
        if (y) out << ',';
        out << '[';
        for (int x = 0; x < BoardWidth; ++x) {
            if (x) out << ',';
            const char cell = y * BoardWidth + x < static_cast<int>(board.size())
                ? board[static_cast<std::size_t>(y * BoardWidth + x)] : '_';
            if (cell == '_' || cell == 'E') out << "null";
            else out << '"' << cell << '"';
        }
        out << ']';
    }
    out << ']';
    return out.str();
}

std::string replaySequence(const std::vector<PageEntry>& pages) {
    std::string sequence;
    std::string previousVisibleQueue;
    for (const auto& page : pages) {
        if (page.next.empty()) continue;
        const std::string active = page.activePiece;
        const std::string visible = active.empty() ? page.next
            : (page.next.rfind(active, 0) == 0 ? page.next.substr(active.size()) : page.next);
        if (sequence.empty()) {
            sequence = active + visible;
            previousVisibleQueue = visible;
            continue;
        }
        std::size_t overlap = 0;
        const std::size_t maxOverlap = std::min(previousVisibleQueue.size(), visible.size());
        for (std::size_t candidate = maxOverlap; candidate > 0; --candidate) {
            if (previousVisibleQueue.compare(previousVisibleQueue.size() - candidate, candidate,
                                             visible, 0, candidate) == 0) {
                overlap = candidate;
                break;
            }
        }
        if (overlap < visible.size()) sequence += visible.substr(overlap);
        previousVisibleQueue = visible;
    }
    return sequence;
}

void writeOperationJson(std::ostringstream& out, const PageEntry& page) {
    if (page.operationPiece.empty()) return;
    out << ",\"o\":{\"type\":\"" << page.operationPiece
        << "\",\"rotation\":\"" << operationRotationName(page.operationRotation)
        << "\",\"x\":" << page.operationX
        << ",\"y\":" << page.operationY;
    out << ",\"coordinateSpace\":\"simulator\"";
    if (page.operationHold) out << ",\"holdUsed\":true";
    out << '}';
}

std::string visibleNextForPage(const PageEntry& page) {
    if (page.activePiece.empty()) return page.next;
    return page.next.rfind(page.activePiece, 0) == 0
        ? page.next.substr(page.activePiece.size())
        : page.next;
}

void writeReplayPageJson(std::ostringstream& out, const PageEntry& page, bool includeOperation = true) {
    const std::string visibleNext = visibleNextForPage(page);
    out << "{\"board\":" << boardMatrixJson(page.board)
        << ",\"active\":\"" << jsonEscape(page.activePiece) << "\""
        << ",\"hold\":\"" << jsonEscape(page.hold)
        << "\",\"next\":\"" << jsonEscape(visibleNext)
        << "\",\"n\":\"" << jsonEscape(page.next) << "\"";
    if (includeOperation) writeOperationJson(out, page);
    out << '}';
}

void writeReplayInitialJson(std::ostringstream& out, const std::vector<PageEntry>& pages) {
    const PageEntry blank;
    const auto& first = pages.empty() ? blank : pages.front();
    out << "{\"board\":" << boardMatrixJson(first.board)
        << ",\"hold\":\"" << jsonEscape(first.hold)
        << "\",\"sequence\":\"" << jsonEscape(replaySequence(pages)) << "\"}";
}

std::string collectionSimulatorJson(const std::vector<PageEntry>& p1,
                                    const std::vector<PageEntry>& p2,
                                    bool twoPlayers) {
    std::ostringstream json;
    json << "{\"v\":3,\"m\":\"" << (twoPlayers ? "2P" : "1P")
         << "\",\"currentCase\":0,\"cases\":[{\"id\":\"video-replay\","
         << "\"name\":\"Video replay\",\"kind\":\"replay\",\"gameMode\":\""
         << (twoPlayers ? "2P" : "1P") << "\",\"initial\":{\"p1\":";
    writeReplayInitialJson(json, p1);
    json << ",\"p2\":";
    writeReplayInitialJson(json, p2);
    json << "},\"pages\":[";

    if (!twoPlayers) {
        for (std::size_t i = 0; i < p1.size(); ++i) {
            if (i) json << ',';
            json << "{\"p1\":";
            writeReplayPageJson(json, p1[i]);
            json << '}';
        }
    } else {
        std::vector<double> times;
        for (const auto& page : p1) times.push_back(page.time);
        for (const auto& page : p2) times.push_back(page.time);
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        std::size_t p1Index = 0;
        std::size_t p2Index = 0;
        bool firstPage = true;
        for (const double time : times) {
            while (p1Index + 1 < p1.size() && p1[p1Index + 1].time <= time) ++p1Index;
            while (p2Index + 1 < p2.size() && p2[p2Index + 1].time <= time) ++p2Index;
            if (p1Index >= p1.size() || p2Index >= p2.size()) continue;
            if (!firstPage) json << ',';
            firstPage = false;
            const auto& a = p1[p1Index];
            const auto& b = p2[p2Index];
            json << "{\"p1\":";
            // A carried page is still the same simulator state. Its
            // highlighted lock must survive a page update belonging only to
            // the other player; otherwise 2P replay makes minos flicker.
            writeReplayPageJson(json, a, true);
            json << ",\"p2\":";
            writeReplayPageJson(json, b, true);
            json << '}';
        }
    }
    json << "]}]}";
    return json.str();
}

std::string simulatorUrl(const std::vector<PageEntry>& p1,
                         const std::vector<PageEntry>& p2,
                         bool twoPlayers) {
    return "https://selmtoe.github.io/Tetris_Simulator/F/index.html#"
        + base64(collectionSimulatorJson(p1, p2, twoPlayers));
}

std::string boardRowsJson(const Board& board) {
    std::ostringstream out;
    out << '[';
    for (int y = 0; y < BoardHeight; ++y) {
        if (y) out << ',';
        out << '"';
        for (int x = 0; x < BoardWidth; ++x) out << cellChar(board[index(x, y)]);
        out << '"';
    }
    out << ']';
    return out.str();
}

std::string garbageMasksJson(const GarbageRise& garbage) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < garbage.holeMasks.size(); ++i) {
        if (i) out << ',';
        out << garbage.holeMasks[i];
    }
    out << ']';
    return out.str();
}

void writeGarbageJson(std::ostringstream& out, const GarbageRise& garbage) {
    out << "{\"lines\":" << garbage.lines
        << ",\"holeMasks\":" << garbageMasksJson(garbage)
        << ",\"matchRatio\":" << std::fixed << std::setprecision(4) << garbage.matchRatio
        << ",\"manual\":" << (garbage.manuallySpecified ? "true" : "false") << '}';
}

void writePlacementJson(std::ostringstream& out, const TimelineStep& step, bool holdUsed) {
    out << "{\"piece\":\"" << sanitizePiece(step.placedPiece)
        << "\",\"x\":" << step.placementX
        << ",\"y\":" << step.placementY
        << ",\"rotation\":" << step.placementRotation
        << ",\"clearedLines\":" << step.clearedLines
        << ",\"coordinateSpace\":\"simulator\""
        << ",\"holdUsed\":" << (holdUsed ? "true" : "false") << '}';
}

void writeTimelineJson(std::ostringstream& out, const std::vector<TimelineStep>& timeline) {
    out << '[';
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        if (i) out << ',';
        const auto& step = timeline[i];
        out << "{\"start\":" << std::fixed << std::setprecision(3) << step.startSeconds
            << ",\"time\":" << step.timeSeconds
            << ",\"active\":\"" << sanitizePiece(step.piece)
            << "\",\"action\":\"" << jsonEscape(step.action)
            << "\",\"hold\":\"" << sanitizePiece(step.hold)
            << "\",\"next\":\"" << sanitizePieces(step.next)
            << "\",\"simulatorNext\":\"" << simulatorQueueForStep(step)
            << "\",\"shiftLines\":" << step.garbage.lines
            << ",\"shiftMatchRatio\":" << step.garbage.matchRatio
            << ",\"garbage\":";
        writeGarbageJson(out, step.garbage);
        out << ",\"placement\":";
        // The placement stored on this row is the lock of the previous
        // timeline state.  Carry that state's HOLD flag with the placement so
        // the standalone recovery JSON remains replayable too.
        writePlacementJson(out, step, i > 0 && timeline[i - 1].holdUsed);
        out
            << ",\"score\":" << step.score
            << ",\"manual\":" << (step.manuallyFixed ? "true" : "false")
            << ",\"queueManual\":" << (step.queueManuallyFixed ? "true" : "false")
            << ",\"observed\":" << boardRowsJson(step.observed)
            << ",\"board\":" << boardRowsJson(step.board)
            << ",\"fullBoard\":" << boardRowsJson(step.fullBoard) << '}';
    }
    out << ']';
}

std::string sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD returned = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;

    const auto close = [&] {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &returned, 0) != 0) {
        close();
        return {};
    }
    object.resize(objectLength);
    digest.resize(hashLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) != 0) {
        close();
        return {};
    }
    std::ifstream source(path, std::ios::binary);
    if (!source) {
        close();
        return {};
    }
    // Keep the hashing chunk off the Windows UI thread's limited stack.  A
    // 1 MiB std::array here can overflow the default stack exactly when the
    // reviewer presses Export.
    std::vector<char> buffer(1024 * 1024);
    while (source) {
        source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = source.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) != 0) {
            close();
            return {};
        }
    }
    if (BCryptFinishHash(hash, digest.data(), hashLength, 0) != 0) {
        close();
        return {};
    }
    close();
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (unsigned char value : digest) {
        result.push_back(Hex[value >> 4]);
        result.push_back(Hex[value & 15]);
    }
    return result;
}

bool writeTrainingAnnotation(const std::filesystem::path& input, const std::filesystem::path& directory,
                              const std::vector<TimelineStep>& p1, const std::vector<TimelineStep>& p2,
                              RecoveryOutput& output, std::string& error) {
    std::error_code ec;
    const auto trainingDirectory = directory / L"training";
    std::filesystem::create_directories(trainingDirectory, ec);
    if (ec) { error = "Could not create training annotation directory: " + ec.message(); return false; }
    const std::string hash = sha256File(input);
    if (hash.empty()) { error = "Could not calculate SHA-256 for the source video"; return false; }
    const std::string shortHash = hash.substr(0, 16);
    const std::wstring wideShortHash(shortHash.begin(), shortHash.end());
    const std::wstring stem = input.stem().wstring();
    std::filesystem::path extension = input.extension();
    if (extension.empty()) extension = L".video";

    output.trainingDatasetDirectory = trainingDirectory;
    output.trainingVideoPath.clear();
    output.trainingAnnotationPath = trainingDirectory / (stem + L"_" + wideShortHash + L"_training.v1.json");
    output.trainingManifestPath = trainingDirectory / (stem + L"_" + wideShortHash + L"_manifest.v1.json");

    std::filesystem::path relativeVideo;
    std::string storage = "external";
    if (output.includeSourceVideoInTraining) {
        const auto videoDirectory = trainingDirectory / L"video";
        std::filesystem::create_directories(videoDirectory, ec);
        if (ec) { error = "Could not create training video directory: " + ec.message(); return false; }
        const auto storedVideo = videoDirectory / (stem + L"_" + wideShortHash + extension.wstring());
        if (std::filesystem::exists(storedVideo, ec)) {
            if (ec) { error = "Could not inspect training video path: " + ec.message(); return false; }
            storage = "existing";
        } else {
            ec.clear();
            // A same-volume hard link is instant and remains valid if the
            // original video is renamed or deleted. Fall back to a normal
            // copy for another volume or a filesystem without hard links.
            std::filesystem::create_hard_link(input, storedVideo, ec);
            if (!ec) {
                storage = "hardlink";
            } else {
                ec.clear();
                std::filesystem::copy_file(input, storedVideo, std::filesystem::copy_options::none, ec);
                if (ec) { error = "Could not save source video for training: " + ec.message(); return false; }
                storage = "copy";
            }
        }
        output.trainingVideoPath = storedVideo;
        relativeVideo = std::filesystem::path("video") / storedVideo.filename();
    }

    const std::string sourceVideoPath = relativeVideo.empty() ? input.generic_u8string() : relativeVideo.generic_u8string();
    const std::string relativeVideoPath = relativeVideo.generic_u8string();
    std::uintmax_t sourceBytes = std::filesystem::file_size(input, ec);
    if (ec) { sourceBytes = 0; ec.clear(); }

    std::ofstream file(output.trainingAnnotationPath);
    if (!file) { error = "Could not write training annotation"; return false; }
    std::ostringstream p1Json;
    std::ostringstream p2Json;
    std::ostringstream p1QueueJson;
    std::ostringstream p2QueueJson;
    writeTimelineJson(p1Json, p1);
    writeTimelineJson(p2Json, p2);
    writeQueueRecognitionJson(p1QueueJson, output.queueObservationsP1);
    writeQueueRecognitionJson(p2QueueJson, output.queueObservationsP2);
    file << "{\"schema\":\"tetris-video-recovery.training/v1\""
         << ",\"source\":{\"videoPath\":\"" << jsonEscape(sourceVideoPath) << "\""
         << ",\"originalVideoPath\":\"" << jsonEscape(input.generic_u8string()) << "\""
         << ",\"sha256\":\"" << hash << "\",\"bytes\":" << sourceBytes << "}"
         << ",\"dataset\":{\"selfContained\":" << (!relativeVideo.empty() ? "true" : "false")
         << ",\"videoFile\":\"" << jsonEscape(relativeVideoPath) << "\""
         << ",\"videoStorage\":\"" << storage << "\"}"
         << ",\"stateConvention\":\"timeline[i] is the board/queue state while active is controlled; placing timeline[i].active produces timeline[i+1].board\""
         << ",\"annotationPolicy\":\"human-approved legal reconstruction; manual=true marks an explicitly chosen candidate\""
         << ",\"review\":{\"approvedByExplicitExport\":" << (output.humanApproved ? "true" : "false") << '}'
         << ",\"queueObservations\":{\"p1\":" << p1QueueJson.str()
         << ",\"p2\":" << p2QueueJson.str() << '}'
         << ",\"players\":{\"p1\":" << p1Json.str() << ",\"p2\":" << p2Json.str() << "}}\n";
    if (!file) { error = "Could not finish training annotation"; return false; }

    std::ofstream manifest(output.trainingManifestPath);
    if (!manifest) { error = "Could not write training manifest"; return false; }
    manifest << "{\"schema\":\"tetris-video-recovery.dataset-manifest/v1\""
             << ",\"annotationFile\":\"" << jsonEscape(output.trainingAnnotationPath.filename().generic_u8string()) << "\""
             << ",\"videoFile\":\"" << jsonEscape(relativeVideoPath) << "\""
             << ",\"videoStorage\":\"" << storage << "\""
             << ",\"sourceSha256\":\"" << hash << "\""
             << ",\"approved\":" << (output.humanApproved ? "true" : "false") << "}\n";
    if (!manifest) { error = "Could not finish training manifest"; return false; }
    return true;
}

bool writeOutputs(const std::filesystem::path& input, const std::filesystem::path& directory,
                  const std::vector<TimelineStep>& p1, const std::vector<TimelineStep>& p2,
                  bool twoPlayers, RecoveryOutput& output, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { error = "Could not create output directory: " + ec.message(); return false; }
    const auto name = input.stem().wstring() + L"_tetris_recovered";
    output.jsonPath = directory / (name + L".json");
    // Real Internet Shortcut files can be double-clicked to open the URL in
    // the user's default browser.  The old .url.txt files only opened a text
    // editor and made the hand-off unnecessarily error-prone.
    output.p1UrlPath = directory / (name + L"_P1.url");
    output.p2UrlPath = directory / (name + L"_P2.url");
    output.combinedUrlPath = directory / (name + L"_2P.url");
    output.linksPath = directory / (name + L"_links.html");
    output.reportPath = directory / (name + L"_report.html");

    auto p1Pages = pageEntries(p1);
    auto p2Pages = pageEntries(p2);
    PageEntry blank;
    blank.board = std::string(BoardWidth * BoardHeight, '_');
    if (p1Pages.empty()) p1Pages.push_back(blank);
    if (p2Pages.empty()) p2Pages.push_back(blank);
    output.p1Url = simulatorUrl(p1Pages, p2Pages, false);
    output.p2Url = simulatorUrl(p2Pages, p1Pages, false);
    output.combinedUrl = twoPlayers ? simulatorUrl(p1Pages, p2Pages, true) : output.p1Url;
    const std::string simulatorData = collectionSimulatorJson(p1Pages, p2Pages, twoPlayers);

    {
        std::ofstream p1File(output.p1UrlPath);
        std::ofstream p2File(output.p2UrlPath);
        std::ofstream combinedFile(output.combinedUrlPath);
        p1File << "[InternetShortcut]\nURL=" << output.p1Url << '\n';
        p2File << "[InternetShortcut]\nURL=" << output.p2Url << '\n';
        combinedFile << "[InternetShortcut]\nURL=" << output.combinedUrl << '\n';
        if (!p1File || !p2File || !combinedFile) { error = "Could not write simulator URL files"; return false; }
    }
    {
        std::ofstream json(output.jsonPath);
        if (!json) { error = "Could not write JSON output"; return false; }
        std::ostringstream p1Json;
        std::ostringstream p2Json;
        writeTimelineJson(p1Json, p1);
        writeTimelineJson(p2Json, p2);
        json << "{\"version\":5,\"pageFormat\":\"operation-pages/v1\",\"input\":\"" << jsonEscape(input.u8string())
             << "\",\"algorithm\":\"video-analysis-html-compatible\",\"p1\":" << p1Json.str()
            << ",\"p2\":" << p2Json.str()
             << ",\"queueObservations\":{\"p1\":";
        writeQueueRecognitionJson(json, output.queueObservationsP1);
        json << ",\"p2\":";
        writeQueueRecognitionJson(json, output.queueObservationsP2);
        json << "}"
             << ",\"simulatorData\":" << simulatorData
             << ",\"urls\":{\"p1\":\"" << jsonEscape(output.p1Url)
             << "\",\"p2\":\"" << jsonEscape(output.p2Url)
             << "\",\"combined\":\"" << jsonEscape(output.combinedUrl) << "\"}}\n";
    }
    if (!writeTrainingAnnotation(input, directory, p1, p2, output, error)) return false;
    {
        std::ofstream report(output.reportPath);
        if (!report) { error = "Could not write HTML report"; return false; }
        report << "<!doctype html><meta charset='utf-8'><title>Tetris Recovery Report</title>"
               << "<style>body{font-family:system-ui;background:#111;color:#eee;padding:20px}table{border-collapse:collapse}td,th{border:1px solid #555;padding:4px}.board{font:12px monospace;white-space:pre}</style>"
               << "<h1>Tetris recovery report</h1><p>Algorithm: original 動画解析.html compatible queue / vote / beam pipeline. Rows marked <em>manual</em> were fixed from a generated legal candidate before export.</p>";
        const auto reportPlayer = [&](const char* title, const std::vector<TimelineStep>& timeline) {
            report << "<h2>" << title << " (" << timeline.size() << " phases)</h2><table><tr><th>#</th><th>start</th><th>end</th><th>active</th><th>action</th><th>hold</th><th>next</th><th>shift</th><th>score</th><th>manual</th><th>board</th></tr>";
            for (std::size_t i = 0; i < timeline.size(); ++i) {
                const auto& step = timeline[i];
                report << "<tr><td>" << i << "</td><td>" << step.startSeconds << "</td><td>" << step.timeSeconds
                       << "</td><td>" << sanitizePiece(step.piece) << "</td><td>" << step.action
                       << "</td><td>" << sanitizePiece(step.hold) << "</td><td>" << sanitizePieces(step.next)
                       << "</td><td>" << step.garbage.lines << "</td><td>" << step.score
                       << "</td><td>" << (step.manuallyFixed ? "yes" : "") << "</td><td class='board'>";
                for (int y = VisibleRows; y < BoardHeight; ++y) {
                    for (int x = 0; x < BoardWidth; ++x) report << cellChar(step.board[index(x, y)]);
                    report << "<br>";
                }
                report << "</td></tr>";
            }
            report << "</table>";
        };
        reportPlayer("P1", p1);
        reportPlayer("P2", p2);
        report << "<h2>Simulator URLs</h2><p><a href='" << jsonEscape(output.p1Url) << "'>P1</a></p>"
               << "<p><a href='" << jsonEscape(output.p2Url) << "'>P2</a></p>"
               << "<p><a href='" << jsonEscape(output.combinedUrl) << "'>2P</a></p>";
    }
    {
        std::ofstream links(output.linksPath);
        if (!links) { error = "Could not write simulator links page"; return false; }
        links << "<!doctype html><meta charset='utf-8'><title>Tetris Simulator links</title>"
              << "<style>body{font-family:system-ui;background:#111;color:#eee;padding:24px}a{display:block;margin:16px 0;color:#7cf;font-size:1.1rem}</style>"
              << "<h1>Tetris Simulator</h1>"
              << "<a href='" << jsonEscape(output.p1Url) << "'>Open P1 simulator</a>"
              << "<a href='" << jsonEscape(output.p2Url) << "'>Open P2 simulator</a>"
              << "<a href='" << jsonEscape(output.combinedUrl) << "'>Open 2P simulator</a>";
        if (!links) { error = "Could not finish simulator links page"; return false; }
    }
    return true;
}

bool parsePair(const std::string& value, std::array<double, 2>& out) {
    std::stringstream stream(value);
    char comma = 0;
    return static_cast<bool>(stream >> out[0] >> comma >> out[1]) && comma == ',';
}

bool parseRect(const std::string& value, LayoutRect& out) {
    std::stringstream stream(value);
    char c1 = 0, c2 = 0, c3 = 0;
    return static_cast<bool>(stream >> out.x >> c1 >> out.y >> c2 >> out.w >> c3 >> out.h) &&
           c1 == ',' && c2 == ',' && c3 == ',';
}

void trimString(std::string& value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
}

} // namespace

bool loadSettings(const std::filesystem::path& path, Settings& settings, std::string& error) {
    if (!std::filesystem::exists(path)) return true;
    std::ifstream file(path);
    if (!file) { error = "Could not open configuration file: " + path.string(); return false; }
    std::string line;
    std::string section;
    while (std::getline(file, line)) {
        trimString(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        std::string key = line.substr(0, equal);
        std::string value = line.substr(equal + 1);
        trimString(key);
        trimString(value);
        try {
            if (section == "video" && key == "sample_interval") settings.sampleIntervalSeconds = std::stod(value);
            else if (section == "video" && key == "queue_confirm") settings.queueConfirmSamples = std::stoi(value);
            else if (section == "video" && key == "queue_beam_width") settings.queueBeamWidth = std::stoi(value);
            else if (section == "video" && key == "queue_7bag") settings.queueUseSevenBag = std::stoi(value) != 0;
            else if (section == "video" && key == "force_exact_onnx") settings.forceExactOnnxMatch = std::stoi(value) != 0;
            else if (section == "video" && key == "onnx_samples") settings.onnxSamples = std::stoi(value);
            else if (section == "video" && key == "beam_width") settings.beamWidth = std::stoi(value);
            else if (section == "video" && key == "shift_threshold") settings.shiftThreshold = std::stod(value);
            else if (section == "video" && key == "score_exact") settings.weights.exactMatch = std::stod(value);
            else if (section == "video" && key == "score_empty") settings.weights.emptyMatch = std::stod(value);
            else if (section == "video" && key == "score_color") settings.weights.colorMismatch = std::stod(value);
            else if (section == "video" && key == "score_missing") settings.weights.missingBlock = std::stod(value);
            else if (section == "video" && key == "score_extra") settings.weights.extraBlock = std::stod(value);
            else if (section == "players" && key == "p1_enabled") settings.player1Enabled = std::stoi(value) != 0;
            else if (section == "players" && key == "p2_enabled") settings.player2Enabled = std::stoi(value) != 0;
            else if ((section == "p1" || section == "p2") && key == "board") {
                parseRect(value, section == "p1" ? settings.p1.board : settings.p2.board);
            } else if ((section == "p1" || section == "p2") && key == "hold") {
                parsePair(value, section == "p1" ? settings.p1.hold : settings.p2.hold);
            } else if ((section == "p1" || section == "p2") && key == "next") {
                // Legacy first-next override. The remaining locations retain
                // the source file's exact 122/175/225/275/325 layout.
                parsePair(value, section == "p1" ? settings.p1.next[0] : settings.p2.next[0]);
            } else if ((section == "p1" || section == "p2") && key.size() == 5 && key.rfind("next", 0) == 0 && key[4] >= '1' && key[4] <= '5') {
                const int index = key[4] - '1';
                parsePair(value, section == "p1" ? settings.p1.next[index] : settings.p2.next[index]);
            }
        } catch (...) {
            error = "Invalid configuration value: " + key;
            return false;
        }
    }
    settings.sampleIntervalSeconds = std::clamp(settings.sampleIntervalSeconds, .001, 1.0);
    settings.queueConfirmSamples = std::clamp(settings.queueConfirmSamples, 2, 6);
    settings.queueBeamWidth = std::clamp(settings.queueBeamWidth, 8, 512);
    settings.onnxSamples = std::clamp(settings.onnxSamples, 1, 20);
    settings.beamWidth = std::clamp(settings.beamWidth, 1, 2000);
    settings.shiftThreshold = std::clamp(settings.shiftThreshold, 0.0, 1.0);
    return true;
}

bool analyzeVideo(const std::filesystem::path& input, const std::filesystem::path& modelPath,
                  const Settings& settings, Status& status, RecoveryOutput& output, std::string& error) {
    // A new analysis starts with no stale file paths/URLs.  The caller may
    // later revise `rawP*` / `p*` in the review editor before exporting.
    output = RecoveryOutput{};
    std::vector<QueuePhase> p1Phases;
    std::vector<QueuePhase> p2Phases;
    std::vector<QueueRecognitionSample> p1QueueSamples;
    std::vector<QueueRecognitionSample> p2QueueSamples;
    double duration = 0;
    if (!collectQueuePhases(input, settings, status, p1Phases, p2Phases,
                            p1QueueSamples, p2QueueSamples, duration, error)) return false;
    output.videoDurationSeconds = duration;
    output.queueObservationsP1 = p1QueueSamples;
    output.queueObservationsP2 = p2QueueSamples;
    output.originalQueueObservationsP1 = p1QueueSamples;
    output.originalQueueObservationsP2 = p2QueueSamples;

    std::vector<std::vector<BoardObservation>> p1Samples;
    std::vector<std::vector<BoardObservation>> p2Samples;
    if (!collectBoardSamples(input, modelPath, settings, p1Phases, p2Phases, status, p1Samples, p2Samples, error)) return false;

    for (const auto& group : p1Samples) {
        output.boardObservationsP1.insert(output.boardObservationsP1.end(), group.begin(), group.end());
    }
    for (const auto& group : p2Samples) {
        output.boardObservationsP2.insert(output.boardObservationsP2.end(), group.begin(), group.end());
    }

    status.setMessage("Pass 3/3: original-compatible legal-move beam search");
    status.progress.store(84);
    output.rawP1 = buildRawTimeline(p1Phases, p1Samples);
    output.rawP2 = buildRawTimeline(p2Phases, p2Samples);
    output.p1 = settings.player1Enabled ? TetrisEngine::beamSearch(output.rawP1, settings) : std::vector<TimelineStep>{};
    status.progress.store(91);
    output.p2 = settings.player2Enabled ? TetrisEngine::beamSearch(output.rawP2, settings) : std::vector<TimelineStep>{};
    status.progress.store(97);
    status.progress.store(100);
    status.setMessage("Analysis complete: review and correct legal candidates before export");
    return true;
}

std::string boardMatrixJson(const std::string& board) {
    std::ostringstream out;
    out << '[';
    for (int y = 0; y < BoardHeight; ++y) {
        if (y) out << ',';
        out << '[';
        for (int x = 0; x < BoardWidth; ++x) {
            if (x) out << ',';
            const char cell = y * BoardWidth + x < static_cast<int>(board.size())
                ? board[static_cast<std::size_t>(y * BoardWidth + x)] : '_';
            if (cell == '_' || cell == 'E') out << "null";
            else out << '"' << cell << '"';
        }
        out << ']';
    }
    out << ']';
    return out.str();
}

bool reanalyzeQueueObservations(const Settings& settings, RecoveryOutput& output, std::string& error) {
    if (output.videoDurationSeconds <= 0) {
        error = "Raw queue log has no video duration";
        return false;
    }
    const auto p1Phases = settings.player1Enabled
        ? buildQueuePhasesFromSamples(output.queueObservationsP1, settings, output.videoDurationSeconds)
        : std::vector<QueuePhase>{};
    const auto p2Phases = settings.player2Enabled
        ? buildQueuePhasesFromSamples(output.queueObservationsP2, settings, output.videoDurationSeconds)
        : std::vector<QueuePhase>{};

    const auto rawP1 = buildRawTimelineFromFlatBoards(p1Phases, output.boardObservationsP1);
    const auto rawP2 = buildRawTimelineFromFlatBoards(p2Phases, output.boardObservationsP2);
    output.rawP1 = rawP1;
    output.rawP2 = rawP2;
    output.p1 = settings.player1Enabled ? TetrisEngine::beamSearch(output.rawP1, settings) : std::vector<TimelineStep>{};
    output.p2 = settings.player2Enabled ? TetrisEngine::beamSearch(output.rawP2, settings) : std::vector<TimelineStep>{};
    return true;
}

bool writeRecoveredOutput(const std::filesystem::path& input, const std::filesystem::path& outputDirectory,
                          const Settings& settings, RecoveryOutput& output, std::string& error) {
    return writeOutputs(input, outputDirectory, output.p1, output.p2,
                        settings.player1Enabled && settings.player2Enabled, output, error);
}

bool recoverVideo(const std::filesystem::path& input, const std::filesystem::path& outputDirectory,
                  const std::filesystem::path& modelPath, const Settings& settings, Status& status,
                  RecoveryOutput& output, std::string& error) {
    if (!analyzeVideo(input, modelPath, settings, status, output, error)) return false;
    status.setMessage("Writing simulator URLs and report");
    if (!writeRecoveredOutput(input, outputDirectory, settings, output, error)) return false;
    status.setMessage("Recovery complete");
    return true;
}

} // namespace tr
