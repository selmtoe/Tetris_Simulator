#pragma once

#include "recovery.hpp"
#include <iomanip>
#include <ostream>

namespace tr {

inline void writeRecoveryQualityIssuesJson(
        std::ostream& out, const std::vector<RecoveryQualityIssue>& issues,
        double sourceStartSeconds) {
    out << '[';
    for (std::size_t index = 0; index < issues.size(); ++index) {
        if (index) out << ',';
        const auto& issue = issues[index];
        out << "{\"player\":" << issue.player
            << ",\"first_phase\":" << issue.firstPhase
            << ",\"last_phase\":" << issue.lastPhase
            << ",\"relative_start_seconds\":" << issue.startSeconds
            << ",\"relative_end_seconds\":" << issue.endSeconds
            << ",\"source_start_seconds\":" << sourceStartSeconds + issue.startSeconds
            << ",\"source_end_seconds\":" << sourceStartSeconds + issue.endSeconds
            << ",\"minimum_match_ratio\":" << issue.minimumMatchRatio
            << ",\"consecutive_phases\":" << issue.consecutivePhases
            << ",\"last_move_phase\":" << issue.lastMovePhase
            << ",\"moves_through_end\":" << issue.movesThroughEnd << '}';
    }
    out << ']';
}

inline void writeRecoveryQualityJson(std::ostream& out,
                                    const RecoveryQualityReport& report,
                                    double sourceStartSeconds) {
    const auto flags = out.flags();
    const auto precision = out.precision();
    out << std::fixed << std::setprecision(6)
        << "{\"suspicious\":" << (report.suspicious() ? "true" : "false")
        << ",\"match_ratio_threshold\":" << report.matchRatioThreshold
        << ",\"required_consecutive_phases\":" << report.requiredConsecutivePhases
        << ",\"clip_scope\":\"whole-match-no-padding\",\"issues\":";
    writeRecoveryQualityIssuesJson(out, report.issues, sourceStartSeconds);
    out << ",\"terminal_anomaly\":{\"present\":"
        << (report.terminalAnomaly() ? "true" : "false")
        << ",\"classification\":\"late-match-end\",\"last_moves\":"
        << RecoveryQualityReport::TerminalMoveLimit
        << ",\"training_target\":\"match-end-detection\""
        << ",\"label_basis\":\"automatic-tail-quality-rule\""
        << ",\"boundary_annotation\":\"required\",\"corrected_end_seconds\":null"
        << ",\"issues\":";
    writeRecoveryQualityIssuesJson(out, report.terminalIssues, sourceStartSeconds);
    out << "}}";
    out.flags(flags);
    out.precision(precision);
}

} // namespace tr
