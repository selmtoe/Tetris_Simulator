#pragma once

#include "recovery.hpp"

namespace tr {

struct TrainingImportInfo {
    std::filesystem::path annotationPath;
    std::filesystem::path suggestedVideoPath;
    std::string sourceSha256;
    std::uintmax_t sourceBytes = 0;
    bool approvedByExplicitExport = false;
    int firstDiscontinuityPlayer = 0;
    std::size_t firstDiscontinuityIndex = 0;
};

// Restore a native review session from either training/v1 or training/v2.
// The returned RecoveryOutput contains both the approved solved timelines and
// their raw observations so queue edits, candidate corrections, continuation,
// and export use the same native paths as a freshly analyzed video.
bool loadTrainingAnnotation(const std::filesystem::path& annotationPath,
                            RecoveryOutput& output,
                            TrainingImportInfo& info,
                            std::string& error);

// Returns the earliest broken board transition across both players. This is
// also used by the native export guard after the reviewer changes candidates.
bool firstTrainingDiscontinuity(const RecoveryOutput& output,
                                int& player,
                                std::size_t& index);

} // namespace tr
