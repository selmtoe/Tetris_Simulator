#include "onnx_model.hpp"
#include "vision.hpp"

#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: onnx_smoke.exe <model.onnx>\n";
        return 2;
    }
    std::string error;
    tr::OnnxBoardModel model(argv[1], error);
    if (!model.ready()) {
        std::cerr << error << "\n";
        return 1;
    }
    std::vector<float> features(200 * 63, 0.0f);
    const auto labels = model.infer(features, error);
    if (!error.empty() || labels.size() != 200) {
        std::cerr << (error.empty() ? "invalid ONNX result" : error) << "\n";
        return 1;
    }

    tr::Settings settings;
    tr::VisionAnalyzer analyzer(1920, 1080, settings.p1, &model);
    tr::Frame frame;
    frame.width = 1920;
    frame.height = 1080;
    frame.bgra.assign(static_cast<std::size_t>(frame.width) * frame.height * 4, 0);
    const auto observation = analyzer.analyze(frame);
    if (!observation.recognitionError.empty()) {
        std::cerr << observation.recognitionError << "\n";
        return 1;
    }
    std::cout << "ONNX model loaded, extracted 12,600 features, and inferred 200 cells successfully.\n";
    return 0;
}
