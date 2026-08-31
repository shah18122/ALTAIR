// app/instruments_demo.hpp — the Phase 1 walkthrough behind `altair --instruments`.
//
// Declaration only. The body is in instruments_demo.cpp so main.cpp stays under
// the PROTOCOL §8 size guide.

#pragma once

#include <cstddef>

namespace altair::demo {

/// Run the Phase 1 instrument-master walkthrough.
///
/// `path` may be nullptr, in which case a small built-in dump is used so the
/// demo runs with no files present. Returns a process exit code.
/// `master_path` may be nullptr, in which case a synthetic exchange master
/// is used. Give it a real UDiFF bhavcopy to reconcile against the real one.
[[nodiscard]] int run_instruments(const char* path, const char* master_path);

} // namespace altair::demo
