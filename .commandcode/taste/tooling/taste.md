# Tooling Preferences

- Uses CMake build presets and ctest test presets as the standard build/test entry points. Confidence: 0.85
- Latency benchmarks are plain executables (analytics/bench, report_latency in core/time tests); Google Benchmark is NOT wired in (find_package(benchmark) is commented out). Confidence: 0.95
- Uses vcpkg with a frozen manifest (`vcpkg.json`); adding any third-party dependency beyond it is a review failure. Confidence: 0.9
- Prefers header-only modules wired as CMake INTERFACE libraries with a namespaced alias (e.g. `altair::types`) and an exported include root so consumers write `#include <types/...>`. Confidence: 0.7
