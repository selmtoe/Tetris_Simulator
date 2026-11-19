#if defined(TR_BROWSER_PARITY_DIAGNOSTIC)
void writeBrowserParityBoards(const std::vector<BoardObservation>& p1,
                             const std::vector<BoardObservation>& p2,
                             const char* suffix = "") {
    const char* path = std::getenv("TR_BROWSER_PARITY_EXPORT");
    if (!path || !*path) return;
    std::ofstream out(std::string(path) + suffix, std::ios::binary); out << std::setprecision(17);
    out << "{\"p1\":[";
    const auto side = [&out](const auto& values) {
        bool first = true;
        for (const auto& b : values) {
            if (!first) out << ','; first = false;
            out << "{\"time\":" << b.timeSeconds << ",\"board\":[";
            for (std::size_t i=0;i<b.board.size();++i) { if(i)out<<',';out<<static_cast<int>(b.board[i]); }
            out << "],\"probabilities\":[";
            for (std::size_t i=0;i<b.probabilities.size();++i) { if(i)out<<',';out<<b.probabilities[i]; }
            out << "],\"confidence\":[";
            for (std::size_t i=0;i<b.confidence.size();++i) { if(i)out<<',';out<<static_cast<int>(b.confidence[i]); }
            out << "],\"quality\":" << b.quality << '}';
        }
    };
    side(p1); out << "],\"p2\":["; side(p2); out << "]}";
}
#endif
