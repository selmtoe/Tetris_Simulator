# Browser video recovery

This directory is the iPad/Safari front end for the native video recovery tool.

The browser pipeline is deliberately two-pass and uses the same C++ sources as
the desktop program:

1. VisionAnalyzer::observeQueue() scans every configured sample interval.
2. prepareObservationRequests() runs the native queue decoder and produces the
   native phase-derived ONNX request times.
3. The browser executes the checked-in tetris.onnx with ONNX Runtime Web.
4. VisionAnalyzer::boardFeatures() and
   VisionAnalyzer::analyzeBoardWithLabels() keep feature extraction,
   classic garbage fallback, and label application in C++.
5. recoverObservations() runs the same queue phase builder, timeline
   construction, garbage handling, and TetrisEngine::beamSearch().
6. writeRecoveredOutput() produces the native JSON, simulator URLs, report,
   and training annotation in the WASM filesystem.

After analysis, the browser stays in the same review stage as the desktop
tool. The phase editor can filter ONNX cells, paint the four placement cells,
select an exact originalLegalMoves() candidate, edit garbage-rise line holes,
and restore or re-run the automatic beam from any phase. The time-based queue
log supports multi-row current/Hold/NEXT edits, restoring raw recognition, and
rebuilding the phase timeline from those edits.

The export button writes the approved recovery JSON, P1/P2/2P simulator links,
the HTML report, training-annotation.json, training-manifest.json, and a
downloadable copy of the source video used by the training dataset. The
annotation is marked human-approved only when that button is pressed.

tetris_recovery.js and tetris_recovery.wasm are generated with Emscripten from
src/recovery.cpp, src/tetris_engine.cpp, src/vision.cpp, src/wasm_bridge.cpp,
and the portable WASM helpers. The ONNX model is loaded from
assets/tetris.onnx.

Run locally from the repository root with any static server:

    python -m http.server 8080

Then open:

    http://localhost:8080/tetris_video_recovery/web/

The native desktop executable remains under tetris_video_recovery/bin/ and the
original simulator remains untouched.

To rebuild the browser bundle after changing the shared C++ bridge:

    powershell -ExecutionPolicy Bypass -File .\tetris_video_recovery\build-web.ps1
