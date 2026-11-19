#pragma once
#include "scene_detector.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <deque>
#include <vector>

namespace tr::scene_detail {
constexpr double EndWindowSeconds = 3.0;
constexpr double EndRatio = 0.70;
constexpr double ReadyEvidenceWindowSeconds = 4.0;
constexpr double ReadyEvidenceMinimumSeconds = 0.10;
constexpr double StrongGoMinimumSeconds = 0.10;
constexpr double StartSearchSeconds = 2.0;
constexpr double CeremonyCooldownSeconds = 5.0;
constexpr double StrongBlackoutMinimumSeconds = 0.25;
// 黒転だけで終了を確定しないための下限（試合中の一瞬の暗転で誤検出しない）
constexpr float StrongBlackoutMinimumEndProbability = 0.25f;

struct Prediction {
    int label = 0;
    float confidence = 0;
    float endProbability = 0;
};

struct Ceremony {
    std::int64_t readyFrame = -1;
    std::int64_t goFirstFrame = -1;
    std::int64_t goLastFrame = -1;
    std::int64_t startFrame = -1;
    const char* startRule = "";
};

inline std::vector<Ceremony> findCeremonies(
        const std::vector<Prediction>& predictions, double fps, bool final = true) {
    std::vector<Ceremony> ceremonies;
    const auto frameCount = static_cast<std::int64_t>(predictions.size());
    const auto readyWindow = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::llround(
            fps * ReadyEvidenceWindowSeconds)));
    const auto readyMinimum = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::ceil(
            fps * ReadyEvidenceMinimumSeconds)));
    const auto goMinimum = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::ceil(
            fps * StrongGoMinimumSeconds)));
    const auto startSearch = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::llround(
            fps * StartSearchSeconds)));
    const auto cooldown = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::llround(
            fps * CeremonyCooldownSeconds)));

    for (std::int64_t goFirst = 0; goFirst < frameCount;) {
        if (predictions[static_cast<std::size_t>(goFirst)].label != 3) {
            ++goFirst;
            continue;
        }
        // TR_GO_BRIDGE=N（既定2、v65〜。0で従来動作）: GO の途中に N フレーム以下の非GOが挟まっても、
        // その後に GO が続けば同じ GO の続きとして扱う（GO 文字が盤面を横切る配信で
        // 1フレームだけ READY と誤判定され、GO 途中から解析が始まるのを防ぐ実験）。
        static const std::int64_t goBridge = [] {
            const char* v = std::getenv("TR_GO_BRIDGE");
            return v ? std::max<std::int64_t>(0, std::atoll(v)) : 2;
        }();
        std::int64_t goEnd = goFirst + 1;
        while (goEnd < frameCount) {
            if (predictions[static_cast<std::size_t>(goEnd)].label == 3) {
                ++goEnd;
                continue;
            }
            std::int64_t next = -1;
            for (std::int64_t k = goEnd + 1;
                 k <= goEnd + goBridge && k < frameCount; ++k) {
                if (predictions[static_cast<std::size_t>(k)].label == 3) {
                    next = k;
                    break;
                }
            }
            if (next < 0) break;
            goEnd = next + 1;
        }
        // A partial GO run or start-search window can still change its start
        // rule, cooldown acceptance and the preceding game's end boundary.
        if (!final && (goEnd == frameCount || goEnd + startSearch > frameCount)) break;
        if (goEnd - goFirst < goMinimum) {
            goFirst = goEnd;
            continue;
        }

        const auto readyBegin = std::max<std::int64_t>(0, goFirst - readyWindow);
        std::int64_t readyFrame = -1;
        std::int64_t readyVotes = 0;
        for (std::int64_t frame = readyBegin; frame < goFirst; ++frame) {
            if (predictions[static_cast<std::size_t>(frame)].label == 2) {
                if (readyFrame < 0) readyFrame = frame;
                ++readyVotes;
            }
        }
        if (readyVotes < readyMinimum) {
            goFirst = goEnd;
            continue;
        }

        const auto searchEnd = std::min(frameCount, goEnd + startSearch);
        // 既定: GOと判定された最後のフレームの次を開始にする（go-exit）。
        // TR_START_RULE=legacy を指定した場合のみ、従来の
        // analysis_start（2連続）/ gameplay フォールバックを使う。
        const bool useGoExit = [] {
            const char* v = std::getenv("TR_START_RULE");
            return !v || std::string(v) != "legacy";
        }();
        std::int64_t startFrame = -1;
        const char* startRule = "go-exit";
        // TR_START_OFFSET_MS=X（既定0）: go-exit の開始を X ミリ秒ずらす（負で前へ）。
        static const double startOffsetMs = [] {
            const char* v = std::getenv("TR_START_OFFSET_MS");
            return v ? std::atof(v) : 0.0;
        }();
        if (useGoExit) {
            startFrame = std::clamp<std::int64_t>(
                goEnd + static_cast<std::int64_t>(std::llround(startOffsetMs * fps / 1000.0)),
                goFirst + 1, frameCount);
        } else {
            int startCount = 0;
            for (std::int64_t frame = goEnd; frame < searchEnd; ++frame) {
                if (predictions[static_cast<std::size_t>(frame)].label == 4) {
                    if (!startCount) startFrame = frame;
                    if (++startCount >= 2) break;
                } else {
                    startCount = 0;
                    startFrame = -1;
                }
            }
            startRule = "post-go-analysis-start";
            if (startCount < 2) {
                startFrame = -1;
                for (std::int64_t frame = goEnd; frame < searchEnd; ++frame) {
                    if (predictions[static_cast<std::size_t>(frame)].label == 5) {
                        startFrame = frame;
                        startRule = "post-go-gameplay-fallback";
                        break;
                    }
                }
            }
        }
        if (startFrame >= 0 &&
            (ceremonies.empty() ||
             startFrame - ceremonies.back().startFrame > cooldown)) {
            ceremonies.push_back({readyFrame, goFirst, goEnd - 1,
                                  startFrame, startRule});
        }
        goFirst = goEnd;
    }
    return ceremonies;
}

inline void assembleMatches(const std::vector<Prediction>& predictions, double fps,
                     int blackoutEndRequired,
                     std::vector<SceneMatchRange>& matches, bool final = true) {
    matches.clear();
    const auto ceremonies = findCeremonies(predictions, fps, final);
    const auto frameCount = static_cast<std::int64_t>(predictions.size());
    const auto endWindowFrames = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::llround(fps * EndWindowSeconds)));
    const auto endRequiredVotes = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::ceil(
            endWindowFrames * EndRatio)));
    const auto strongBlackoutRequired = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::ceil(
            fps * StrongBlackoutMinimumSeconds)));

    // （終了票の数え方は下の isEnd を参照。1.5秒ルールは廃止した）

    for (std::size_t index = 0; index < ceremonies.size(); ++index) {
        if (!final && index + 1 == ceremonies.size()) break;
        const auto& ceremony = ceremonies[index];
        const auto limit = index + 1 < ceremonies.size()
            ? ceremonies[index + 1].readyFrame : frameCount;
        bool gameplaySeen = false;
        std::int64_t endRunStart = -1;
        std::int64_t lastEndFrame = -1;
        std::deque<std::pair<bool, float>> endWindow;
        std::int64_t endVotes = 0;
        double endProbabilitySum = 0;
        std::int64_t blackoutFirst = -1;
        std::int64_t blackoutCount = 0;
        std::int64_t endFrame = -1;
        float endConfidence = 0;
        const char* endRule = "";

        for (std::int64_t frame = ceremony.startFrame;
             frame < limit; ++frame) {
            const auto& prediction = predictions[static_cast<std::size_t>(frame)];
            if (!gameplaySeen) {
                if (prediction.label == 5) gameplaySeen = true;
                continue;
            }
            // 6/8/9（終了・引き分け・勝敗つき終了）は同じ「終了」として数える。
            // 勝敗を1本のモデルで出す場合、結果画面は 80/90 が中心になり、
            // 汎用の 60_end はほとんど出ない。以前は「1.5秒以上つづく高確信度の
            // 8/9」だけを数えていたため、結果画面の確信度が揺らぐ配信で終了が
            // 数秒〜数十秒ずれた。誤検出は3秒窓の70%ルールで抑える。
            const bool isEnd = (prediction.label == 6 || prediction.label == 8 ||
                                prediction.label == 9) &&
                prediction.confidence >= 0.30f;
            // 終了は「結果画面が始まった瞬間」を採る（70%窓の確定時刻ではなく）。
            // TR_END_BRIDGE=N（既定2、v65〜。0で従来動作）: 結果画面の連続の途中に N フレーム以下の非終了が
            // 挟まっても連続が切れたとみなさない（1フレームだけ gameplay に戻る誤判定で
            // 終了が遅れ、傾いた盤面まで解析するのを防ぐ実験）。
            static const std::int64_t endBridge = [] {
                const char* v = std::getenv("TR_END_BRIDGE");
                return v ? std::max<std::int64_t>(0, std::atoll(v)) : 2;
            }();
            if (isEnd) {
                if (endRunStart < 0) endRunStart = frame;
                lastEndFrame = frame;
            } else if (endRunStart >= 0 && frame - lastEndFrame > endBridge) {
                endRunStart = -1;
            }
            endWindow.emplace_back(isEnd, prediction.endProbability);
            endVotes += isEnd ? 1 : 0;
            endProbabilitySum += prediction.endProbability;
            if (endWindow.size() > static_cast<std::size_t>(endWindowFrames)) {
                endVotes -= endWindow.front().first ? 1 : 0;
                endProbabilitySum -= endWindow.front().second;
                endWindow.pop_front();
            }
            if (endWindow.size() >= static_cast<std::size_t>(endWindowFrames) &&
                endVotes >= endRequiredVotes) {
                endFrame = endRunStart >= 0 ? endRunStart : frame;
                endRule = "3s-window-70pct";
            }

            if (prediction.label == 7) {
                if (!blackoutCount) blackoutFirst = frame;
                ++blackoutCount;
                if (endFrame < 0 && endVotes >= blackoutEndRequired) {
                    endFrame = blackoutFirst;
                    endRule = "blackout-with-end-evidence";
                } else if (endFrame < 0 &&
                           blackoutCount >= strongBlackoutRequired) {
                    const double windowEndProbability = endWindow.empty()
                        ? 0.0
                        : endProbabilitySum / static_cast<double>(endWindow.size());
                    if (windowEndProbability >=
                        static_cast<double>(StrongBlackoutMinimumEndProbability)) {
                        endFrame = blackoutFirst;
                        endRule = "sustained-blackout-recovery";
                    }
                }
            } else {
                blackoutFirst = -1;
                blackoutCount = 0;
            }
            if (endFrame >= 0) {
                endConfidence = endWindow.empty() ? 0.0f : static_cast<float>(
                    endProbabilitySum / static_cast<double>(endWindow.size()));
                break;
            }
        }

        if (endFrame < 0 && index + 1 < ceremonies.size() &&
            limit > ceremony.startFrame) {
            // A complete next READY -> GO ceremony proves that the preceding
            // game ended even when the end/blackout image classes both failed.
            endFrame = limit;
            endRule = "next-ceremony-recovery";
        }
        if (endFrame <= ceremony.startFrame) continue;

        SceneMatchRange match;
        match.index = static_cast<int>(matches.size() + 1);
        match.readyFrame = ceremony.readyFrame;
        match.goFirstFrame = ceremony.goFirstFrame;
        match.goLastFrame = ceremony.goLastFrame;
        match.startFrame = ceremony.startFrame;
        match.endFrame = endFrame;
        match.startSeconds = static_cast<double>(match.startFrame) / fps;
        match.endSeconds = static_cast<double>(match.endFrame) / fps;
        match.startConfidence = predictions[
            static_cast<std::size_t>(match.startFrame)].confidence;
        match.endConfidence = endConfidence;
        match.startRule = ceremony.startRule;
        match.endRule = endRule;
        matches.push_back(std::move(match));
    }
}


} // namespace tr::scene_detail
