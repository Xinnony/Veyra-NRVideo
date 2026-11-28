// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace veyra::ui::tts {
// The Piper voice process (veyra_qml_ui --tts-worker). Reads JSON lines on
// stdin: first {"folder":..., "threads":n}, then {"speak":text},
// {"cancel":true} or {"volume":0..1}. Writes {"ready":true}, {"error":...}
// and {"speaking":bool} lines on stdout. Exits when stdin closes.
int runWorker();
}
