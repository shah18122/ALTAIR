# Tooling Preferences

- Uses CMake build presets and ctest test presets as the standard build/test entry points. Confidence: 0.85
- Uses Google Benchmark for latency regression measurements. Confidence: 0.85
- Uses vcpkg with a frozen manifest (`vcpkg.json`); adding any third-party dependency beyond it is a review failure. Confidence: 0.9
- Prefers header-only modules wired as CMake INTERFACE libraries with a namespaced alias (e.g. `altair::types`) and an exported include root so consumers write `#include <types/...>`. Confidence: 0.7
