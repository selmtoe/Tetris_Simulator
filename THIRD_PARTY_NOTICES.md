# Third-party notices

## sfinder-cpp — MIT

The perfect-clear (PC) search uses the C++ core of
[sfinder-cpp](https://github.com/knewjade/sfinder-cpp) by knewjade.
Copyright (c) 2019 knewjade. This is sfinder-cpp, not the separate Java
solution-finder project.

- Reference source: `third_party/sfinder-cpp-master/`.
- Browser wrapper: `simulator/pc-solver/pc_solver_api.cpp` (project code).
- Distributed build: `simulator/pc-solver/sfinder-pc.js` and `sfinder-pc.wasm`.
- Full MIT copyright, permission and warranty notice: [licenses/sfinder-MIT.txt](licenses/sfinder-MIT.txt), also preserved in the reference source and generated JavaScript.
- Source and build script: [licenses/sfinder-source.zip](licenses/sfinder-source.zip).

Keep the MIT notice with copies or substantial portions, including binary
packages. The license text is reproduced verbatim; wrapper/UI files do not
alter the upstream copyright attribution.

## Cold Clear — Mozilla Public License 2.0

This project uses [Cold Clear](https://github.com/MinusKelvin/cold-clear)
by MinusKelvin and contributors, including the Rust Standard search DAG,
evaluator, normal-mode search, libtetris board/move rules, and opening-book
code. This is use of source code, not only copied AI parameters.

The following source and modifications are provided under MPL-2.0:

- `third_party/cold-clear-reference/`, including project modifications.
- `simulator/cold-clear-wasm/`, the Rust C ABI integration.
- `simulator/workers/cold-clear-core.js`, the JavaScript port used by REN search and comparisons.

The compiled `simulator/workers/cold-clear.wasm` contains this covered code.
`cold-clear-wasm.js` and `cold-clear-wasm-worker.js` connect it to the browser.
Consumers, including the simulator, viewer, local Lab analysis, and KASANE
research that uses libtetris, retain the same license for the covered files.
Linking independent files does not place all project files or private data
under MPL. Existing upstream notices remain intact.

- [Full MPL-2.0 text](licenses/cold-clear-MPL-2.0.txt), also preserved in `third_party/cold-clear-reference/LICENSE` and [LICENSE](LICENSE).
- [Source of the distributed WASM, including modifications and build scripts](licenses/cold-clear-source.zip), available without charge.
- [Current JavaScript port, in editable source form](simulator/workers/cold-clear-core.js).
- [Integration and modification notes](simulator/COLD_CLEAR_PORT.md).

No additional restriction is imposed on recipients' MPL rights in the source.
The upstream software is supplied subject to the warranty disclaimers and
liability limitations in the respective licenses. Original notices must be
preserved when redistributing covered source or its modifications.

## Source delivery and builds

The public [license page](licenses/index.html) links to the license texts and
source packages on the same site. The simulator and viewer link to that page
from their Share dialogs, also available when embedded in Hub or Lab.

`licenses/*-build.json` records hashes of each binary and its build-time source
ZIP. The ZIP is authoritative for that binary; the development checkout may
contain newer edits. JavaScript is distributed directly in source form.
Public repository history is also available at
<https://github.com/selmtoe/Tetris_Simulator>.

After compilation, `tools/build-cold-clear-wasm.ps1` and
`tools/build-pc-solver-wasm.ps1` automatically preserve the relevant source,
license, and build instructions with `tools/package-ai-licenses.py`.
`tools/build-web.py` refuses a missing or mismatched source package and copies
all notices and packages to the public artifact. The Pages workflow runs
license and artifact checks before deployment. Include these same notices
and source packages when redistributing the engines outside that artifact.

This document covers sfinder-cpp and Cold Clear. Other dependencies retain
their own licenses.

Compiler runtime and transitive dependency notices are also distributed as
[licenses/sfinder-dependencies.txt](licenses/sfinder-dependencies.txt) and
[licenses/cold-clear-dependencies.txt](licenses/cold-clear-dependencies.txt).
These retain their original licenses and are not relicensed as MPL.
