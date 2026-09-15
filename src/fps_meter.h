// fps_meter.h - lightweight per-frame rate meter for verifying FPS changes.
//
// Strong override of the recompiled main-loop function MainRenderLoop_82B9CD68
// (0x82B9CD68; renamed from sub_82B9CD68 in fable_2_manifest.toml). One
// call per frame; ~33 ms period at the 30 fps cap, see
// docs/FPS_CAP_INVESTIGATION.md). Counts invocations over 5 s windows and
// appends `mainloop rate=NN.N/s` lines to fps_meter.log (CWD = exe dir).
// Forward-only: the override just counts, then calls the original
// __imp__ entry point, so it is safe to leave compiled in.
//
// Enabled with the FABLE2_FPS_METER=1 environment variable. Off by default.
// Include this header in the app's main TU (main.cpp) - the strong override
// below then binds in place of the weak recompiled symbol.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include <rex/ppc/func.h>

namespace fable2::fpsmeter {

inline bool enabled() {
  static const bool e = [] {
    const char* v = std::getenv("FABLE2_FPS_METER");
    return v && v[0] == '1';
  }();
  return e;
}

}  // namespace fable2::fpsmeter

extern "C" void MainRenderLoop_82B9CD68(PPCContext& ctx, uint8_t* base) {
  if (fable2::fpsmeter::enabled()) {
    static std::atomic<uint64_t> calls{0};
    static std::atomic<int64_t> window_start_us{0};
    static std::atomic<uint64_t> window_calls{0};
    static std::ofstream& logf = [] -> std::ofstream& {
      std::remove("fps_meter.log");  // fresh log per run
      static std::ofstream f{"fps_meter.log", std::ios::app};
      return f;
    }();
    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    calls.fetch_add(1, std::memory_order_relaxed);
    int64_t start = window_start_us.load(std::memory_order_relaxed);
    if (start == 0)
      window_start_us.compare_exchange_strong(start, now_us);
    const uint64_t n = window_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (now_us - start) >= 5'000'000) {
      window_calls.store(0, std::memory_order_relaxed);
      window_start_us.store(now_us, std::memory_order_relaxed);
      const double secs = (now_us - start) / 1'000'000.0;
      if (logf) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "mainloop rate=%.1f/s (total %llu)\n",
                      n / secs,
                      (unsigned long long)calls.load(std::memory_order_relaxed));
        logf << buf;
        logf.flush();
      }
    }
  }
  __imp__MainRenderLoop_82B9CD68(ctx, base);
}
