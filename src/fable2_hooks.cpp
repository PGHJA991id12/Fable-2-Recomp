// fable2_hooks.cpp - mid-asm hook functions for the [[entrypoint.midasm_hook]]
// entries in fable_2_manifest.toml (see docs/patches.md).
//
// Codegen emits an extern prototype for each hook into the generated code
// (e.g. `extern void fable2_hook_60fps(PPCRegister& r11);`) and calls it at
// the configured instruction address, passing the named registers BY
// REFERENCE, so a hook can read and/or rewrite them. Define each hook with
// plain C++ linkage (NOT extern "C") and exactly once, matching the emitted
// prototype.

#include <rex/ppc/context.h>  // PPCRegister (union with u64/s64/u32/... views)

#include "fable2_config.h"

// 60 FPS (Xenia "60 FPS" by Margen67; guest: be8 0x82B9C8EB = 0x01).
// Injected right after `li r11,2` (0x82B9C8E8) in the state-2 case of
// sub_82B9C7F8: overwrites r11 = 2 with 1, mirroring the guest byte flip.
// Measured effect: main loop 30/s -> ~60/s (FABLE2_FPS_METER=1).
//
// Toggle: [patches] fps_60 in fable2_config.toml (default true), consulted
// on every call - flip it and relaunch to A/B the patch, no rebuild. The
// config is loaded in OnPostInitLogging, well before any guest code runs.
void fable2_hook_60fps(PPCRegister& r11) {
  if (fable2::config::Get().fps_60) {
    r11.s64 = 1;
  }
}
