// Included by recovery.cpp inside its anonymous namespace, after the
// authoritative queue decoder.  This is a scheduler-side cache only: final
// recovery still calls buildQueuePhasesFromSamples on the complete history.

bool sameIncrementalQueueRun(const QueueRun& left, const QueueRun& right) {
    return left.first == right.first && left.last == right.last &&
           left.manual == right.manual &&
           samePieces(left.observation.next, right.observation.next);
}

bool sameIncrementalLogicalRun(const LogicalQueueRun& left,
                               const LogicalQueueRun& right) {
    return left.first == right.first && left.last == right.last &&
           left.confirmed == right.confirmed && left.queue == right.queue &&
           left.observedHold == right.observedHold &&
           left.holdIsAnchor == right.holdIsAnchor &&
           left.holdEvidenceReliable == right.holdEvidenceReliable &&
           left.holdEvidenceSamples == right.holdEvidenceSamples &&
           left.weightByEvidenceSamples == right.weightByEvidenceSamples;
}

template <typename T, typename Equal>
std::size_t incrementalCommonPrefix(const std::vector<T>& left,
                                    const std::vector<T>& right, Equal equal) {
    std::size_t result = 0;
    while (result < left.size() && result < right.size() &&
           equal(left[result], right[result])) {
        ++result;
    }
    return result;
}

struct IncrementalPhaseDecoderStats {
    std::uint64_t queueRefreshes = 0;
    std::uint64_t queueRollbacks = 0;
    std::uint64_t queueReplayedEvents = 0;
    std::size_t queueMaxDirtySuffix = 0;
    std::uint64_t logicalRefreshes = 0;
    std::uint64_t logicalRollbacks = 0;
    std::uint64_t logicalReplayedEvents = 0;
    std::size_t logicalMaxDirtySuffix = 0;
};

class IncrementalGlobalQueueDecoder {
public:
    explicit IncrementalGlobalQueueDecoder(const Settings& settings)
        : settings_(settings) {}

    void reset(const std::vector<QueueRecognitionSample>& samples) {
        samples_ = samples;
        queueRuns_.clear();
        const auto first = std::find_if(samples_.begin(), samples_.end(),
            [](const QueueRecognitionSample& sample) {
                return hasFullQueue(sample.observation);
            });
        if (first != samples_.end()) {
            queueRuns_ = queueRunsFromSamples(
                samples_, static_cast<std::size_t>(std::distance(samples_.begin(), first)));
        }
        queueNodes_.clear();
        queueLayers_.clear();
        decodedQueuePath_.clear();
        logicalRuns_.clear();
        logicalNodes_.clear();
        logicalLayers_.clear();
        logicalPath_.clear();
        stats_ = {};
        queueDirty_ = queueRuns_.empty() ? std::optional<std::size_t>{}
                                         : std::optional<std::size_t>{0};
        queuePathDirtyEvent_.reset();
        logicalInputDirtySample_ = samples_.empty() ? std::optional<std::size_t>{}
                                                     : std::optional<std::size_t>{0};
        materializedSampleCount_ = 0;
        forceFullMaterialize_ = true;
    }

    void push(double time, const QueueObservation& observation) {
        samples_.push_back({time, Cell::Empty, observation});
        appendQueueRun(samples_.back());
    }

    void pushSample(const QueueRecognitionSample& sample) {
        samples_.push_back(sample);
        appendQueueRun(samples_.back());
        forceFullMaterialize_ = forceFullMaterialize_ || sample.manuallyEdited;
    }

    std::vector<QueuePhase> preview(double duration) {
        updateQueueFrontier();
        materializeQueuePath();
        updateLogicalFrontier();
        return materializeLogicalPhases(duration);
    }

    const std::vector<QueueRecognitionSample>& samples() const { return samples_; }
    const IncrementalPhaseDecoderStats& stats() const { return stats_; }

private:
    void appendQueueRun(const QueueRecognitionSample& sample) {
        const std::size_t sampleIndex = samples_.size() - 1;
        if (queueRuns_.empty()) {
            if (!hasFullQueue(sample.observation)) return;
            queueRuns_.push_back({sampleIndex, sampleIndex, sample.observation,
                                  sample.manuallyEdited});
            queueDirty_ = 0;
            return;
        }
        const bool split = sample.manuallyEdited || queueRuns_.back().manual ||
            !samePieces(sample.observation.next, queueRuns_.back().observation.next);
        if (split) {
            queueRuns_.push_back({sampleIndex, sampleIndex, sample.observation,
                                  sample.manuallyEdited});
        } else {
            queueRuns_.back().last = sampleIndex;
        }
        const std::size_t changed = queueRuns_.size() - 1;
        queueDirty_ = queueDirty_ ? std::min(*queueDirty_, changed) : changed;
    }

    std::vector<QueueSequenceNode> makeSeedCandidates(const QueueRun& run) const {
        std::vector<QueueSequenceNode> candidates;
        const auto initial = queueWindow(run.observation);
        std::array<Cell, QueueWindowSize> window = initial;
        const int correctionBudget = run.manual ? 0 : 1;
        const auto generate = [&](auto&& self, std::size_t slot, int edits) -> void {
            if (slot == window.size()) {
                for (int phase = 0; phase < (settings_.queueUseSevenBag ? 7 : 1); ++phase) {
                    QueueSequenceNode node;
                    node.window = window;
                    if (!initializeSevenBag(node.window, phase, settings_.queueUseSevenBag,
                                            node.nextBagOffset, node.currentBagMask)) {
                        continue;
                    }
                    node.score = queueEmissionScore(node.window, run) -
                                 edits * SpeculativeSeedPenalty;
                    node.event = 0;
                    node.lastTrustedEvent = 0;
                    node.speculativeSeed = edits > 0;
                    candidates.push_back(node);
                }
                return;
            }
            const Cell observed = initial[slot];
            window[slot] = observed;
            self(self, slot + 1, edits);
            if (edits >= correctionBudget) return;
            for (Cell piece : Pieces) {
                if (piece == observed) continue;
                window[slot] = piece;
                self(self, slot + 1, edits + 1);
            }
            window[slot] = observed;
        };
        generate(generate, 0, 0);
        pruneQueueBeam(candidates, settings_.queueBeamWidth);
        return candidates;
    }

    std::vector<QueueSequenceNode> expandQueueLayer(
        const std::vector<int>& parents, const QueueRun& run,
        std::size_t event) const {
        std::vector<QueueSequenceNode> candidates;
        candidates.reserve(parents.size() * 16);
        for (const int parentIndex : parents) {
            const QueueSequenceNode& parent =
                queueNodes_[static_cast<std::size_t>(parentIndex)];
            for (int advance = 0; advance <= 2; ++advance) {
                std::vector<QueueSequenceNode> frontier{parent};
                for (int shift = 0; shift < advance; ++shift) {
                    std::vector<QueueSequenceNode> appended;
                    appended.reserve(frontier.size() * Pieces.size());
                    for (const auto& state : frontier) {
                        for (Cell piece : Pieces) {
                            QueueSequenceNode next = state;
                            if (appendQueuePiece(next, piece,
                                                 settings_.queueUseSevenBag)) {
                                appended.push_back(next);
                            }
                        }
                    }
                    frontier = std::move(appended);
                    if (frontier.empty()) break;
                }
                for (auto& state : frontier) {
                    const int mismatchLimit = run.manual ? 0 : 1;
                    if (state.speculativeSeed &&
                        !samePieces(queuePieces(state.window), run.observation.next)) {
                        continue;
                    }
                    if (queueMismatchCount(state.window, run) > mismatchLimit) continue;
                    state.parent = parentIndex;
                    state.event = static_cast<int>(event);
                    state.advance = advance;
                    state.totalAdvance = parent.totalAdvance + advance;
                    state.lastTrustedEvent = static_cast<int>(event);
                    state.bagRephase = false;
                    state.score = parent.score + queueEmissionScore(state.window, run) -
                                  (advance == 1 ? .75 : advance == 2 ? 2.25 : 0.0);
                    if (state.speculativeSeed &&
                        samePieces(queuePieces(state.window), run.observation.next)) {
                        state.speculativeSeed = false;
                    }
                    candidates.push_back(std::move(state));
                }
            }
            const int trustedEvent = parent.lastTrustedEvent;
            const bool hasTrustedEvent = trustedEvent >= 0 &&
                static_cast<std::size_t>(trustedEvent) < event;
            const bool priorWasCorrected = hasTrustedEvent &&
                queueMismatchCount(parent.window,
                    queueRuns_[static_cast<std::size_t>(trustedEvent)]) > 0;
            const std::size_t stableSamples = run.last - run.first + 1;
            const double stableGap = hasTrustedEvent
                ? samples_[run.first].timeSeconds -
                  samples_[queueRuns_[static_cast<std::size_t>(trustedEvent)].first].timeSeconds
                : 0.0;
            bool hasExactVisualSlide = false;
            if (hasFullQueue(run.observation)) {
                const auto rawTarget = queueWindow(run.observation);
                for (int advance = 1;
                     advance < static_cast<int>(QueueWindowSize); ++advance) {
                    hasExactVisualSlide = hasExactVisualSlide ||
                        queueOverlapMismatch(parent.window, rawTarget, advance) == 0;
                }
            }
            if (stableQueueResyncEnabled() && !run.manual &&
                settings_.queueUseSevenBag &&
                (priorWasCorrected || hasExactVisualSlide) &&
                stableSamples >= 10 && stableGap >= .30 && hasFullQueue(run.observation)) {
                const auto target = queueWindow(run.observation);
                for (int advance = 1; advance <= static_cast<int>(QueueWindowSize); ++advance) {
                    const int overlapMismatch = queueOverlapMismatch(parent.window, target, advance);
                    const int mismatchLimit = priorWasCorrected ? 1 : 0;
                    if (overlapMismatch > mismatchLimit ||
                        (advance == static_cast<int>(QueueWindowSize) &&
                         !priorWasCorrected)) continue;
                    for (int phase = 0; phase < 7; ++phase) {
                        QueueSequenceNode reset = parent;
                        reset.window = target;
                        if (!initializeSevenBag(reset.window, phase, true,
                                                reset.nextBagOffset,
                                                reset.currentBagMask)) continue;
                        reset.parent = parentIndex;
                        reset.event = static_cast<int>(event);
                        reset.advance = advance;
                        reset.totalAdvance = parent.totalAdvance + advance;
                        reset.lastTrustedEvent = static_cast<int>(event);
                        reset.bagRephase = true;
                        reset.speculativeSeed = false;
                        reset.score = parent.score + queueEmissionScore(reset.window, run)
                            - 3.0 - advance * .75 - overlapMismatch * 1.5;
                        candidates.push_back(std::move(reset));
                    }
                }
            }
            if (!run.manual) {
                QueueSequenceNode fallback = parent;
                fallback.parent = parentIndex;
                fallback.event = static_cast<int>(event);
                fallback.advance = 0;
                fallback.totalAdvance = parent.totalAdvance;
                fallback.bagRephase = false;
                fallback.score = parent.score + queueEmissionScore(parent.window, run) - 20.0;
                candidates.push_back(std::move(fallback));
            }
        }
        pruneQueueBeam(candidates, settings_.queueBeamWidth);
        return candidates;
    }

    void appendQueueLayer(std::vector<QueueSequenceNode> candidates) {
        std::vector<int> layer;
        layer.reserve(candidates.size());
        for (auto& candidate : candidates) {
            layer.push_back(static_cast<int>(queueNodes_.size()));
            queueNodes_.push_back(std::move(candidate));
        }
        queueLayers_.push_back(std::move(layer));
    }

    void updateQueueFrontier() {
        ++stats_.queueRefreshes;
        const auto previousPath = decodedQueuePath_;
        std::size_t dirty = queueDirty_.value_or(queueRuns_.size());
        // If a prior manual contradiction stopped the beam early, the first
        // missing layer is dirty even when its raw run did not change.
        const std::size_t oldLayers = queueLayers_.size();
        dirty = std::min(dirty, oldLayers);
        if (dirty < oldLayers) ++stats_.queueRollbacks;
        queueDirty_.reset();
        decodedQueuePath_.assign(queueRuns_.size(), {});
        if (queueRuns_.empty()) {
            queueNodes_.clear();
            queueLayers_.clear();
            return;
        }

        std::size_t replayed = 0;
        if (dirty == 0) {
            queueNodes_.clear();
            queueLayers_.clear();
            auto seeds = makeSeedCandidates(queueRuns_.front());
            if (seeds.empty()) return;
            appendQueueLayer(std::move(seeds));
            ++replayed;
            dirty = 1;
        } else {
            queueLayers_.resize(dirty);
        }

        for (std::size_t event = dirty; event < queueRuns_.size(); ++event) {
            if (queueLayers_.empty() || queueLayers_.back().empty()) break;
            auto candidates = expandQueueLayer(queueLayers_.back(), queueRuns_[event], event);
            if (candidates.empty()) break;
            appendQueueLayer(std::move(candidates));
            ++replayed;
        }
        stats_.queueReplayedEvents += replayed;
        stats_.queueMaxDirtySuffix = std::max(stats_.queueMaxDirtySuffix, replayed);
        if (queueLayers_.empty() || queueLayers_.back().empty()) return;
        int nodeIndex = queueLayers_.back().front();
        while (nodeIndex >= 0) {
            const auto& node = queueNodes_[static_cast<std::size_t>(nodeIndex)];
            if (node.event >= 0 &&
                static_cast<std::size_t>(node.event) < decodedQueuePath_.size()) {
                decodedQueuePath_[static_cast<std::size_t>(node.event)] = node.window;
            }
            nodeIndex = node.parent;
        }
        const std::size_t pathDirty = incrementalCommonPrefix(
            previousPath, decodedQueuePath_,
            [](const auto& left, const auto& right) { return left == right; });
        if (pathDirty < decodedQueuePath_.size()) {
            queuePathDirtyEvent_ = queuePathDirtyEvent_
                ? std::min(*queuePathDirtyEvent_, pathDirty) : pathDirty;
        }
    }

    void materializeQueuePath() {
        if (!forceFullMaterialize_) {
            std::size_t firstSample = materializedSampleCount_;
            if (queuePathDirtyEvent_ && *queuePathDirtyEvent_ < queueRuns_.size()) {
                firstSample = std::min(firstSample,
                                       queueRuns_[*queuePathDirtyEvent_].first);
            }
            firstSample = std::max(firstSample, preserveBefore_);
            if (firstSample < samples_.size()) {
                for (std::size_t event = 0; event < queueRuns_.size(); ++event) {
                    if (queueRuns_[event].last < firstSample) continue;
                    const auto correctedNext = queuePieces(decodedQueuePath_[event]);
                    const std::size_t begin = std::max(firstSample, queueRuns_[event].first);
                    for (std::size_t i = begin; i <= queueRuns_[event].last; ++i) {
                        auto& sample = samples_[i];
                        sample.decoded = sample.observation;
                        sample.decoded.next = correctedNext;
                        sample.sequenceCorrected =
                            !samePieces(sample.observation.next, correctedNext);
                        const int mismatchLimit = queueRuns_[event].manual ? 0 : 1;
                        sample.rejected = !validQueue(sample.observation) ||
                            queueMismatchCount(decodedQueuePath_[event],
                                               queueRuns_[event]) > mismatchLimit;
                    }
                }
                logicalInputDirtySample_ = logicalInputDirtySample_
                    ? std::min(*logicalInputDirtySample_, firstSample) : firstSample;
            }
            materializedSampleCount_ = samples_.size();
            queuePathDirtyEvent_.reset();
            return;
        }
        struct Snapshot {
            QueueObservation decoded;
            bool sequenceCorrected = false;
            bool holdCorrected = false;
            bool rejected = false;
        };
        std::vector<Snapshot> previous(samples_.size());
        std::uint64_t newestManualRevision = 0;
        std::size_t newestManualIndex = samples_.size();
        std::size_t legacyManualIndex = samples_.size();
        for (std::size_t i = 0; i < samples_.size(); ++i) {
            previous[i] = {samples_[i].decoded, samples_[i].sequenceCorrected,
                           samples_[i].holdCorrected, samples_[i].rejected};
            if (!samples_[i].manuallyEdited) continue;
            legacyManualIndex = std::min(legacyManualIndex, i);
            if (samples_[i].manualRevision > newestManualRevision) {
                newestManualRevision = samples_[i].manualRevision;
                newestManualIndex = i;
            } else if (samples_[i].manualRevision == newestManualRevision &&
                       samples_[i].manualRevision != 0) {
                newestManualIndex = std::min(newestManualIndex, i);
            }
        }
        const std::size_t preserveBefore = newestManualRevision != 0
            ? newestManualIndex : legacyManualIndex;
        preserveBefore_ = preserveBefore < samples_.size() ? preserveBefore : 0;
        for (auto& sample : samples_) {
            sample.decoded = {};
            sample.sequenceCorrected = false;
            sample.holdCorrected = false;
            sample.activeCorrected = false;
            sample.rejected = false;
        }
        for (std::size_t event = 0; event < queueRuns_.size(); ++event) {
            const auto correctedNext = queuePieces(decodedQueuePath_[event]);
            for (std::size_t i = queueRuns_[event].first;
                 i <= queueRuns_[event].last; ++i) {
                auto& sample = samples_[i];
                sample.decoded = sample.observation;
                sample.decoded.next = correctedNext;
                sample.sequenceCorrected =
                    !samePieces(sample.observation.next, correctedNext);
                const int mismatchLimit = queueRuns_[event].manual ? 0 : 1;
                sample.rejected = !validQueue(sample.observation) ||
                    queueMismatchCount(decodedQueuePath_[event], queueRuns_[event]) >
                        mismatchLimit;
            }
        }
        for (std::size_t i = 0; i < preserveBefore && i < samples_.size(); ++i) {
            if (previous[i].decoded.next.empty()) continue;
            samples_[i].decoded = previous[i].decoded;
            samples_[i].sequenceCorrected = previous[i].sequenceCorrected;
            samples_[i].holdCorrected = previous[i].holdCorrected;
            samples_[i].rejected = previous[i].rejected;
        }
        logicalInputDirtySample_ = samples_.empty() ? std::optional<std::size_t>{}
                                                     : std::optional<std::size_t>{0};
        materializedSampleCount_ = samples_.size();
        queuePathDirtyEvent_.reset();
        forceFullMaterialize_ = false;
    }

    std::vector<LogicalQueueRun> logicalRunsFrom(std::size_t firstSample,
                                                 bool logicalHead) const {
        std::vector<LogicalQueueRun> result;
        for (std::size_t i = firstSample; i < samples_.size(); ++i) {
            const auto& sample = samples_[i];
            if (!hasFullQueue(sample.decoded)) continue;
            const auto queue = queueWindow(sample.decoded);
            const Cell hold = logicalHoldEvidence(sample);
            const bool anchor = sample.manuallyEdited;
            const bool reliableHold = holdSampleIsEvidence(
                sample,
                settings_.queueUnclearHoldIsEvidence &&
                    !holdUnclearNotEvidenceEnabled());
            const bool split = result.empty() || result.back().queue != queue ||
                result.back().observedHold != hold || result.back().holdIsAnchor || anchor;
            if (split) {
                result.push_back({i, i, i, queue, hold, anchor, reliableHold,
                                  static_cast<std::size_t>(reliableHold ? 1 : 0),
                                  !(settings_.queueUnclearHoldIsEvidence &&
                                    !holdUnclearNotEvidenceEnabled())});
            } else {
                result.back().last = i;
                result.back().holdEvidenceReliable =
                    result.back().holdEvidenceReliable || reliableHold;
                if (reliableHold) ++result.back().holdEvidenceSamples;
            }
        }
        for (auto& run : result) {
            run.weightByEvidenceSamples =
                !(settings_.queueUnclearHoldIsEvidence &&
                  !holdUnclearNotEvidenceEnabled());
            const std::size_t needed = static_cast<std::size_t>(
                std::max(1, settings_.queueConfirmSamples) - 1);
            run.confirmed = run.holdIsAnchor ? run.first
                                             : std::min(run.last, run.first + needed);
        }
        if (logicalHead && !result.empty()) {
            const auto& first = samples_[result.front().first];
            const Cell suspicious = result.front().observedHold;
            const bool duplicatedHead = first.timeSeconds <= .050001 &&
                isPiece(suspicious) && suspicious == result.front().queue.front() &&
                !result.front().holdIsAnchor;
            if (duplicatedHead) {
                for (auto& run : result) {
                    if (run.observedHold != suspicious || run.holdIsAnchor) break;
                    run.holdEvidenceReliable = false;
                }
            }
        }
        return result;
    }

    std::vector<LogicalQueueNode> expandLogicalLayer(
        const std::vector<int>& parents, const LogicalQueueRun& run,
        std::size_t event) const {
        std::vector<LogicalQueueNode> candidates;
        for (const int parentIndex : parents) {
            const auto& parent = logicalNodes_[static_cast<std::size_t>(parentIndex)];
            const int advance = exactQueueAdvance(parent.state.queue, run.queue);
            if (advance < 0) continue;
            std::vector<std::pair<LogicalQueueState,
                                  std::vector<LogicalQueueOperation>>> transitions;
            enumerateLogicalTransitions(parent.state, run.queue, advance, transitions);
            for (auto& transition : transitions) {
                const double emission = logicalHoldScore(transition.first.hold, run);
                if (emission <= -999999.0) continue;
                const int holds = static_cast<int>(std::count_if(
                    transition.second.begin(), transition.second.end(),
                    [](const auto operation) {
                        return operation == LogicalQueueOperation::Hold ||
                               operation == LogicalQueueOperation::HoldPending;
                    }));
                LogicalQueueNode node;
                node.state = transition.first;
                node.score = parent.score + emission - holds * .35 +
                             logicalOperationOrderScore(transition.second);
                node.parent = parentIndex;
                node.event = static_cast<int>(event);
                node.queueAdvance = advance;
                node.totalHolds = parent.totalHolds + holds;
                node.operations = std::move(transition.second);
                candidates.push_back(std::move(node));
            }
        }
        pruneLogicalQueueBeam(candidates, std::max(32, settings_.queueBeamWidth));
        return candidates;
    }

    void appendLogicalLayer(std::vector<LogicalQueueNode> candidates) {
        std::vector<int> layer;
        layer.reserve(candidates.size());
        for (auto& candidate : candidates) {
            layer.push_back(static_cast<int>(logicalNodes_.size()));
            logicalNodes_.push_back(std::move(candidate));
        }
        logicalLayers_.push_back(std::move(layer));
    }

    void updateLogicalFrontier() {
        ++stats_.logicalRefreshes;
        std::vector<LogicalQueueRun> nextRuns;
        if (!logicalInputDirtySample_) {
            nextRuns = logicalRuns_;
        } else if (logicalRuns_.empty() || *logicalInputDirtySample_ == 0) {
            nextRuns = logicalRunsFrom(0, true);
        } else {
            const std::size_t dirtySample = *logicalInputDirtySample_;
            std::size_t firstDirtyRun = 0;
            while (firstDirtyRun < logicalRuns_.size() &&
                   logicalRuns_[firstDirtyRun].last < dirtySample) {
                ++firstDirtyRun;
            }
            if (firstDirtyRun == logicalRuns_.size()) {
                firstDirtyRun = logicalRuns_.empty() ? 0 : logicalRuns_.size() - 1;
            } else if (firstDirtyRun > 0) {
                --firstDirtyRun;
            }
            const std::size_t firstSample = logicalRuns_.empty()
                ? 0 : logicalRuns_[firstDirtyRun].first;
            nextRuns.assign(logicalRuns_.begin(),
                            logicalRuns_.begin() + static_cast<std::ptrdiff_t>(firstDirtyRun));
            auto suffix = logicalRunsFrom(firstSample, firstDirtyRun == 0);
            nextRuns.insert(nextRuns.end(),
                            std::make_move_iterator(suffix.begin()),
                            std::make_move_iterator(suffix.end()));
        }
        logicalInputDirtySample_.reset();
        std::size_t dirty = incrementalCommonPrefix(
            logicalRuns_, nextRuns, sameIncrementalLogicalRun);
        const std::size_t oldLayers = logicalLayers_.size();
        dirty = std::min(dirty, oldLayers);
        if (dirty < oldLayers) ++stats_.logicalRollbacks;
        logicalRuns_ = std::move(nextRuns);
        logicalPath_.assign(logicalRuns_.size(), -1);
        if (logicalRuns_.empty()) {
            logicalNodes_.clear();
            logicalLayers_.clear();
            return;
        }

        std::size_t replayed = 0;
        if (dirty == 0) {
            logicalNodes_.clear();
            logicalLayers_.clear();
            LogicalQueueNode seed;
            seed.state.queue = logicalRuns_.front().queue;
            seed.state.hold = logicalRuns_.front().holdEvidenceReliable
                ? logicalRuns_.front().observedHold : Cell::Empty;
            seed.score = logicalHoldScore(seed.state.hold, logicalRuns_.front());
            seed.event = 0;
            appendLogicalLayer({std::move(seed)});
            ++replayed;
            dirty = 1;
        } else {
            logicalLayers_.resize(dirty);
        }

        for (std::size_t event = dirty; event < logicalRuns_.size(); ++event) {
            if (logicalLayers_.empty() || logicalLayers_.back().empty()) break;
            auto candidates = expandLogicalLayer(logicalLayers_.back(), logicalRuns_[event], event);
            if (candidates.empty()) break;
            appendLogicalLayer(std::move(candidates));
            ++replayed;
        }
        stats_.logicalReplayedEvents += replayed;
        stats_.logicalMaxDirtySuffix = std::max(stats_.logicalMaxDirtySuffix, replayed);
        if (logicalLayers_.size() != logicalRuns_.size() ||
            logicalLayers_.empty() || logicalLayers_.back().empty()) {
            return;
        }
        int nodeIndex = logicalLayers_.back().front();
        while (nodeIndex >= 0) {
            const auto& node = logicalNodes_[static_cast<std::size_t>(nodeIndex)];
            logicalPath_[static_cast<std::size_t>(node.event)] = nodeIndex;
            nodeIndex = node.parent;
        }
    }

    std::vector<QueuePhase> materializeLogicalPhases(double duration) {
        if (logicalRuns_.empty() || logicalPath_.size() != logicalRuns_.size() ||
            std::any_of(logicalPath_.begin(), logicalPath_.end(),
                        [](int value) { return value < 0; })) {
            QueuePhaseBuilder fallback(settings_.queueConfirmSamples);
            for (const auto& sample : samples_) {
                fallback.push(sample.timeSeconds,
                    sample.decoded.next.empty() ? sample.observation : sample.decoded);
            }
            return fallback.preview(duration);
        }

        const double firstTime = samples_[logicalRuns_.front().confirmed].timeSeconds;
        LogicalQueueState replay = logicalNodes_[
            static_cast<std::size_t>(logicalPath_.front())].state;
        std::vector<QueuePhase> phases;
        phases.push_back({0, firstTime, replay.hold, queuePieces(replay.queue),
                          Cell::Empty, "init", false, false});
        double currentStart = firstTime;
        for (std::size_t event = 1; event < logicalRuns_.size(); ++event) {
            const auto& node = logicalNodes_[
                static_cast<std::size_t>(logicalPath_[event])];
            const double eventTime =
                samples_[logicalRuns_[event].confirmed].timeSeconds;
            int consumed = 0;
            bool spawnedInThisEvent = false;
            int locksInThisEvent = 0;
            for (const auto operation : node.operations) {
                if (operation == LogicalQueueOperation::Lock) {
                    std::string action = "place";
                    if (spawnedInThisEvent && currentStart == eventTime) {
                        action = "spawn_skipped";
                    } else if (locksInThisEvent > 0 && currentStart == eventTime) {
                        action = "place_skipped";
                    }
                    phases.push_back({currentStart, eventTime, replay.hold,
                                      queuePieces(replay.queue), replay.active,
                                      action, false, replay.activeFromHold});
                    currentStart = eventTime;
                    ++locksInThisEvent;
                } else if (operation == LogicalQueueOperation::Spawn) {
                    spawnedInThisEvent = true;
                    if (!replay.pendingFirstHold) currentStart = eventTime;
                }
                replay = applyLogicalOperation(replay, operation,
                    logicalRuns_[event].queue, node.queueAdvance, consumed);
            }
        }
        if (currentStart < duration || phases.empty()) {
            phases.push_back({currentStart, duration, replay.hold,
                              queuePieces(replay.queue), replay.active,
                              "end", false, replay.activeFromHold});
        }
        return phases;
    }

    Settings settings_;
    std::vector<QueueRecognitionSample> samples_;
    std::vector<QueueRun> queueRuns_;
    std::vector<QueueSequenceNode> queueNodes_;
    std::vector<std::vector<int>> queueLayers_;
    std::vector<std::array<Cell, QueueWindowSize>> decodedQueuePath_;
    std::vector<LogicalQueueRun> logicalRuns_;
    std::vector<LogicalQueueNode> logicalNodes_;
    std::vector<std::vector<int>> logicalLayers_;
    std::vector<int> logicalPath_;
    IncrementalPhaseDecoderStats stats_;
    std::optional<std::size_t> queueDirty_;
    std::optional<std::size_t> queuePathDirtyEvent_;
    std::optional<std::size_t> logicalInputDirtySample_;
    std::size_t materializedSampleCount_ = 0;
    std::size_t preserveBefore_ = 0;
    bool forceFullMaterialize_ = true;
};
