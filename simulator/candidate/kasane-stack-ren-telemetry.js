/* Event-specific Stack-REN strategyDetail decoder shared by candidate Web tools. */

'use strict';

(function publish(root, factory) {
    const api = factory();
    if (typeof module === 'object' && module.exports) module.exports = api;
    if (root) root.KasaneStackRenTelemetry = api;
})(typeof self !== 'undefined' ? self : globalThis, () => {
    const EVENTS = Object.freeze({
        enter: 'Stack-REN enter',
        build: 'Stack-REN build',
        fire: 'Stack-REN fire',
        continue: 'Stack-REN continue',
        abort: 'Stack-REN abort'
    });
    const EVENT_SET = new Set(Object.values(EVENTS));
    const FIRE_REASONS = Object.freeze([
        'ValueStop',
        'Survival',
        'Pressure',
        'NoSafeBuild'
    ]);
    const DEPTH_MASK = 0x1f;
    const REASON_MASK = 0xe0;
    const BUILD_OVER_FIRE = 1 << 5;
    const NO_SAFE_ESCAPE = 3 << 5;
    const FUNCTIONAL_STAGES = Object.freeze({
        idle: 'Idle',
        entered: 'Entered',
        built: 'Built',
        normalFired: 'NormalFired'
    });

    function decode(strategyEvent, strategyDetail) {
        if (!EVENT_SET.has(strategyEvent) || !Number.isInteger(strategyDetail)) return null;
        const rawDetail = strategyDetail & 0xff;
        const decoded = {
            event: strategyEvent,
            rawDetail,
            depth: null,
            fireReasonCode: null,
            fireReason: null,
            declinedProvedFire: false,
            noSafeEscape: false,
            abortReason: null
        };

        // Enter is the only event whose byte encodes well coordinates. Its
        // upper nibble must never be interpreted as a fire/build reason.
        if (strategyEvent === EVENTS.enter) {
            const start = rawDetail & 0x0f;
            const width = (rawDetail >>> 4) & 0x0f;
            decoded.well = {
                start,
                width,
                startColumn: start + 1,
                endColumn: start + width,
                valid: width >= 2 && width <= 4 && start + width <= 10
            };
            return decoded;
        }

        decoded.depth = rawDetail & DEPTH_MASK;
        if (strategyEvent === EVENTS.fire) {
            const reasonCode = (rawDetail & REASON_MASK) >>> 5;
            decoded.fireReasonCode = reasonCode;
            decoded.fireReason = FIRE_REASONS[reasonCode] || null;
        } else if (strategyEvent === EVENTS.build) {
            decoded.declinedProvedFire = (rawDetail & BUILD_OVER_FIRE) !== 0;
        } else if (strategyEvent === EVENTS.abort) {
            decoded.noSafeEscape = (rawDetail & REASON_MASK) === NO_SAFE_ESCAPE;
            decoded.abortReason = decoded.noSafeEscape ? 'NoSafeEscape' : null;
        }
        return decoded;
    }

    function zeroHistogram() {
        return Array(32).fill(0);
    }

    function createSummary() {
        return {
            // Runtime proof state. Call observeExecuted for every executed
            // lock, passing null for a non-Stack-REN lock, so an intervening
            // decision cannot leave a stale partial sequence alive.
            functionalStage: FUNCTIONAL_STAGES.idle,
            functionalSequences: 0,
            executed: {
                entries: 0,
                builds: 0,
                fires: 0,
                continues: 0,
                aborts: 0
            },
            maxDepth: 0,
            buildDepthMax: 0,
            fires: 0,
            valueStopFires: 0,
            survivalFires: 0,
            pressureFires: 0,
            noSafeBuildFires: 0,
            declinedProvedFireBuilds: 0,
            noSafeEscapeAborts: 0,
            fireDepthSum: 0,
            fireDepthMax: 0,
            fireDepthHistogram: zeroHistogram(),
            fireReasonDepthHistograms: {
                ValueStop: zeroHistogram(),
                Survival: zeroHistogram(),
                Pressure: zeroHistogram(),
                NoSafeBuild: zeroHistogram()
            }
        };
    }

    function observeFunctionalSequence(summary, decoded) {
        const previous = Object.values(FUNCTIONAL_STAGES).includes(summary.functionalStage)
            ? summary.functionalStage
            : FUNCTIONAL_STAGES.idle;
        const event = decoded && decoded.event;
        const completed = previous === FUNCTIONAL_STAGES.normalFired
            && event === EVENTS.continue;

        if (event === EVENTS.enter) {
            // A newly executed Enter always starts a fresh proof.
            summary.functionalStage = FUNCTIONAL_STAGES.entered;
        } else if (
            event === EVENTS.build
            && (previous === FUNCTIONAL_STAGES.entered || previous === FUNCTIONAL_STAGES.built)
        ) {
            // Enter itself is not the required post-entry Build.
            summary.functionalStage = FUNCTIONAL_STAGES.built;
        } else if (
            event === EVENTS.fire
            && previous === FUNCTIONAL_STAGES.built
            && (decoded.fireReason === 'ValueStop' || decoded.fireReason === 'NoSafeBuild')
        ) {
            // Survival and Pressure are emergency REN ignitions, not proof
            // that the learned normal firing policy completed a sequence.
            summary.functionalStage = FUNCTIONAL_STAGES.normalFired;
        } else {
            // This also consumes the successful Continue, preventing a
            // repeated Continue from counting the same fire twice.
            summary.functionalStage = FUNCTIONAL_STAGES.idle;
        }

        if (completed) summary.functionalSequences++;
    }

    function observeExecuted(summary, decoded) {
        if (!summary) return summary;
        observeFunctionalSequence(summary, decoded);
        if (decoded?.event === EVENTS.enter) summary.executed.entries++;
        if (decoded?.event === EVENTS.build) summary.executed.builds++;
        if (decoded?.event === EVENTS.fire) summary.executed.fires++;
        if (decoded?.event === EVENTS.continue) summary.executed.continues++;
        if (decoded?.event === EVENTS.abort) summary.executed.aborts++;
        if (!decoded || decoded.depth === null) return summary;
        const depth = decoded.depth;
        summary.maxDepth = Math.max(summary.maxDepth, depth);

        if (decoded.event === EVENTS.build) {
            summary.buildDepthMax = Math.max(summary.buildDepthMax, depth);
            if (decoded.declinedProvedFire) summary.declinedProvedFireBuilds++;
        } else if (decoded.event === EVENTS.abort) {
            if (decoded.noSafeEscape) summary.noSafeEscapeAborts++;
        } else if (decoded.event === EVENTS.fire) {
            summary.fires++;
            summary.fireDepthSum += depth;
            summary.fireDepthMax = Math.max(summary.fireDepthMax, depth);
            summary.fireDepthHistogram[depth]++;
            if (decoded.fireReason) {
                summary.fireReasonDepthHistograms[decoded.fireReason][depth]++;
            }
            if (decoded.fireReason === 'ValueStop') summary.valueStopFires++;
            if (decoded.fireReason === 'Survival') summary.survivalFires++;
            if (decoded.fireReason === 'Pressure') summary.pressureFires++;
            if (decoded.fireReason === 'NoSafeBuild') summary.noSafeBuildFires++;
        }
        return summary;
    }

    return Object.freeze({
        EVENTS,
        FIRE_REASONS,
        DEPTH_MASK,
        REASON_MASK,
        BUILD_OVER_FIRE,
        NO_SAFE_ESCAPE,
        FUNCTIONAL_STAGES,
        decode,
        createSummary,
        observeExecuted
    });
});
