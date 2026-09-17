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

// Unlock Website Items (Xenia "Unlock Website Items" by Guy).
// GATE 1 (bit6 of *(r4+0x90)): the `rlwinm r9, r10, 0, 0x19, 0x19` at 0x8256E384
// extracts bit 6 into r9 (0x40 or 0). Force r9 = 1 so the gate always passes.
//
// Toggle: [patches] unlock_website in fable2_config.toml (default true).
void fable2_hook_website_g1(PPCRegister& r9) {
  LogTrace("website GATE1 (r9->1)");
  if (fable2::config::Get().unlock_website) {
    r9.u64 = 1;
  }
}

// Unlock Website Items. GATE 1b (bit0 of *(r4+0x40)): the `clrlwi r8, r9, 0x1f`
// at 0x8256E3AC extracts bit 0 into r8 (1 or 0). Force r8 = 1 so the gate
// always passes.
//
// Toggle: [patches] unlock_website in fable2_config.toml (default true).
void fable2_hook_website_g1b(PPCRegister& r8) {
  LogTrace("website GATE1b (r8->1)");
  if (fable2::config::Get().unlock_website) {
    r8.u64 = 1;
  }
}

// Unlock Collectors Edition Content (Xenia "Unlock Collectors Edition Content"
// by Guy). GATE 1 (bit6 of *(r4+0x90)): the `rlwinm r10, r11, 0, 0x19, 0x19`
// at 0x824B3540 extracts bit 6 into r10 (0x40 or 0). Force r10 = 1 so the gate
// always passes.
//
// Toggle: [patches] unlock_ce in fable2_config.toml (default true).
void fable2_hook_ce_g1(PPCRegister& r10) {
  LogTrace("ce GATE1 (r10->1)");
  if (fable2::config::Get().unlock_ce) {
    r10.u64 = 1;
  }
}

// Unlock Collectors Edition Content. GATE 1b (bit of *(r4+0x28)): the
// `rlwinm r9, r10, 7, 0x1f, 0x1f` at 0x824B3568 extracts the bit into r9
// (1 or 0). Force r9 = 1 so the gate always passes.
//
// Toggle: [patches] unlock_ce in fable2_config.toml (default true).
void fable2_hook_ce_g1b(PPCRegister& r9) {
  LogTrace("ce GATE1b (r9->1)");
  if (fable2::config::Get().unlock_ce) {
    r9.u64 = 1;
  }
}

// --- TEMPORARY grant/tail diagnostic traces (website getter) ----------------
// 0x8256E3B8 = item-lookup entry; r10 is the item-list pointer (NULL = bad).
void fable2_trace_website_lookup(PPCRegister& r10) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 40)
    REXSYS_INFO("[fable2-trace] website LOOKUP entry, item-list ptr r10=0x{:x}",
                static_cast<unsigned long long>(r10.u64));
}
// 0x8256D9E4 = final result of the grant/dedup method sub_8256D940
// (website-chest vtable[1] "is this item new?" check). Force it to report the
// item as newly granted so the getter (GetWebsiteItem) returns non-zero.
void fable2_hook_website_grantnew(PPCRegister& r3) {
  if (!fable2::config::Get().unlock_website) return;
  r3.u32 = 1;
  LogTrace("website GRANT-NEW forced r3=1");
}
// 0x8256E4C8 = right before the grant bctrl; r11 = grant method address
// (vtable[1]), r3 = the item. Names the grant method so we can inspect why it
// returns 0.
void fable2_trace_website_grantcall(PPCRegister& r11, PPCRegister& r3) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 40)
    REXSYS_INFO("[fable2-trace] website GRANT-CALL method=0x{:x} item=0x{:x}",
                static_cast<unsigned long long>(r11.u64),
                static_cast<unsigned long long>(r3.u64));
}
// 0x8256E4D0 = grant epilogue (reached only if the bctrl grant ran). r3 is the
// grant's return value.
void fable2_trace_website_grant(PPCRegister& r3) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 40)
    REXSYS_INFO("[fable2-trace] website GRANT ran, r3(ret)=0x{:x}",
                static_cast<unsigned long long>(r3.u64));
}
// 0x8256E4E0 = return-0 tail (reached only if a gate FAILED).
void fable2_trace_website_tail() {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 40)
    REXSYS_INFO("[fable2-trace] website TAIL (gate failed, no grant)");
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
void fable2_trace_GetWebsiteItem(PPCRegister& r12) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 20)
    REXSYS_INFO("[fable2-trace] GetWebsiteItem caller LR=0x{:x}",
                static_cast<unsigned long long>(r12.u64));
}
void fable2_trace_GetCEContent(PPCRegister& r12) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 20)
    REXSYS_INFO("[fable2-trace] GetCEContent caller LR=0x{:x}",
                static_cast<unsigned long long>(r12.u64));
}
void fable2_trace_8256E5B0() { LogTrace("8256E5B0"); }
void fable2_trace_8256E888() { LogTrace("8256E888"); }
