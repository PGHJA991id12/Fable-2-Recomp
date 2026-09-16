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
#include <rex/logging/macros.h>

#include <atomic>
#include <string_view>

#include "fable2_config.h"

namespace {
// TEMPORARY chest-click tracing. Shared budget so a burst of calls can't flood
// the log. Remove together with the fable2_trace_* hooks once the fable2.com
// gate is understood.
void LogTrace(std::string_view name) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 400)
    REXSYS_INFO("[fable2-trace] {}", name);
}
}  // namespace

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

// Unlock Website Items (Xenia "Unlock Website Items" by Guy). sub_8256E368 is
// the "is a website/Guild-chest item available" check. It has several gates
// (bit6 of obj+0x90, bit0 of obj+0x40, then a type-0xf0 item lookup) and every
// failure path funnels to the return-0 tail `li r3,0` at 0x8256E4E0. The stock
// guest patch (be8 0x8256E4E3=0x01 + be32 0x8256E4B8) makes that tail return 1
// (and, in this build's layout, always route to it). We do the faithful minimum:
// inject right after the tail's `li r3,0` and force r3 = 1, so the function
// reports "available" whenever the lookup would otherwise fail. A real item
// found returns via the vtable call, not the tail, so that path is untouched.
// (Confirmed in-game: bit6 was already set on chest click, so the binding gate
// is the later lookup - forcing bit6 alone did not open the chest.)
//
// Toggle: [patches] unlock_website in fable2_config.toml (default true).
void fable2_hook_unlock_website(PPCRegister& r3) {
  LogTrace("unlock_website TAIL (r3->1)");
  if (fable2::config::Get().unlock_website) {
    r3.u64 = 1;
  }
}

// Unlock Collectors Edition Content (Xenia "Unlock Collectors Edition Content"
// by Guy). Same shape as the website function: sub_824B3528 is the "is
// CE-chest content available" check with several gates (bit6 of obj+0x90, bit0
// of obj+0x28, then a type-0x39 item lookup) and every failure path funnels to
// the return-0 tail `li r3,0` at 0x824B368C. We inject right after the tail's
// `li r3,0` and force r3 = 1 so the function reports "available" whenever the
// lookup would otherwise fail. (The stock CE patch, be32 0x824B366C=0x39200001,
// targets a different build's layout and does not map to this build.)
//
// Toggle: [patches] unlock_ce in fable2_config.toml (default true).
void fable2_hook_unlock_ce(PPCRegister& r3) {
  LogTrace("unlock_ce TAIL (r3->1)");
  if (fable2::config::Get().unlock_ce) {
    r3.u64 = 1;
  }
}

// --- TEMPORARY chest-click trace hooks (see the matching [[entrypoint.midasm_hook]]
// entries in fable_2_manifest.toml). No-arg entry hooks over the website-chest
// object's sibling methods; each logs its address once per call. REMOVE after
// the fable2.com registration gate is identified and bypassed.
void fable2_trace_8256DC10() { LogTrace("8256DC10"); }
void fable2_trace_8256DCA0() { LogTrace("8256DCA0"); }
void fable2_trace_8256DED8() { LogTrace("8256DED8"); }
void fable2_trace_8256E120() { LogTrace("8256E120"); }
void fable2_trace_8256E4F8(PPCRegister& r12) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 20)
    REXSYS_INFO("[fable2-trace] 8256E4F8 caller LR=0x{:x}",
                static_cast<unsigned long long>(r12.u64));
}
void fable2_trace_82228830(PPCRegister& r12) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 20)
    REXSYS_INFO("[fable2-trace] 82228830 caller LR=0x{:x}",
                static_cast<unsigned long long>(r12.u64));
}
void fable2_trace_8256E5B0() { LogTrace("8256E5B0"); }
void fable2_trace_8256E888() { LogTrace("8256E888"); }
