// fable2_state_probe.h - per-frame UI state sampler for the remote "state" API.
//
// Reads the per-frame UI text list (guest 0x83334AA0; descriptor layout from
// fable2_ui_text_dump.h) on the render thread, extracts the ASCII + UTF-16BE
// strings currently being rendered, and derives the current boot/menu state:
//
//   PreMainMenu | PressAScreen | MainMenuMovie | MainMenu
//
//   PreMainMenu  - splash/logos before the "Press A" prompt
//   PressAScreen - the "Press A to start" prompt is on screen
//   MainMenuMovie- idle past the prompt: the background movie plays
//   MainMenu     - A pressed in time: the interactive main menu
//
// The latest sample is kept in a small thread-safe snapshot the remote control
// server reads on `state` commands. The render thread is the ONLY writer.
//
// Investigation: FABLE2_STATE_PROBE=1 logs every sample (strided) to
// fable2_state_probe.log so the classifier thresholds can be tuned.
//   FABLE2_STATE_PROBE_STRIDE=N  log every Nth sampled frame (default 15)

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/logging/macros.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fable2::stateprobe {

constexpr uintptr_t kArena = 0x100000000ull;
constexpr uint32_t kList = 0x83334AA0u;  // UI text list descriptor (per frame)

// State ids (stable values used by the remote API + the snapshot).
enum State : int {
  kUnknown = 0,
  kPreMainMenu = 1,
  kPressAScreen = 2,
  kMainMenuMovie = 3,
  kMainMenu = 4,
};

inline const char* StateName(int s) {
  switch (s) {
    case kPreMainMenu: return "PreMainMenu";
    case kPressAScreen: return "PressAScreen";
    case kMainMenuMovie: return "MainMenuMovie";
    case kMainMenu: return "MainMenu";
    default: return "Unknown";
  }
}

inline bool logging() {
  static const bool e = [] {
#ifdef _WIN32
    char v[8] = {};
    size_t n = 0;
    return ::getenv_s(&n, v, sizeof(v), "FABLE2_STATE_PROBE") == 0 && v[0] == '1';
#else
    const char* v = std::getenv("FABLE2_STATE_PROBE");
    return v && v[0] == '1';
#endif
  }();
  return e;
}

inline long stride() {
#ifdef _WIN32
  static const long v = [] {
    char b[16] = {};
    size_t n = 0;
    if (::getenv_s(&n, b, sizeof(b), "FABLE2_STATE_PROBE_STRIDE") != 0 || !b[0])
      return 15L;
    return std::atol(b);
  }();
#else
  static const long v = [] {
    const char* e = std::getenv("FABLE2_STATE_PROBE_STRIDE");
    return e ? std::atol(e) : 15;
  }();
#endif
  return v;
}

inline bool page_ok(const void* p) {
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION m;
  if (::VirtualQuery(const_cast<void*>(p), &m, sizeof m) == 0) return false;
  return m.State == MEM_COMMIT &&
         (m.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY |
                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
#else
  (void)p;
  return true;
#endif
}

// Safe guest read from the fixed 0x100000000 arena (commit-on-fault).
inline bool aread(uint32_t ga, void* dst, size_t n) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(kArena + ga);
  size_t off = 0;
  while (off < n) {
    size_t chunk = 0x1000u - ((reinterpret_cast<uintptr_t>(p + off)) & 0xFFFu);
    if (chunk > n - off) chunk = n - off;
    if (!page_ok(p + off)) return false;
    std::memcpy(static_cast<uint8_t*>(dst) + off, p + off, chunk);
    off += chunk;
  }
  return true;
}
inline uint32_t aread32(uint32_t ga) {
  uint32_t v = 0;
  if (!aread(ga, &v, 4)) return 0;
  return __builtin_bswap32(v);
}
// The guest address space is mapped linearly to the 0x100000000 arena and
// spans far more than 0x80000000-0x84000000 (heap scan showed committed
// regions at 0x42xxxxxx .. 0x92xxxxxx). A pointer is "plausible" if its arena
// page is committed + readable.
inline bool plausible(uint32_t a) {
  if (a < 0x00400000u || a >= 0xC0000000u) return false;
  return page_ok(reinterpret_cast<const void*>(kArena + a));
}

// Pull ASCII + UTF-16BE printable runs (>= minlen) out of a buffer.
inline void extract_strings(const uint8_t* p, size_t n,
                            std::vector<std::string>& out, int minlen = 3) {
  size_t i = 0;
  while (i < n) {
    if (p[i] >= 0x20 && p[i] < 0x7F) {
      size_t j = i;
      while (j < n && p[j] >= 0x20 && p[j] < 0x7F) ++j;
      if (j - i >= static_cast<size_t>(minlen))
        out.emplace_back(reinterpret_cast<const char*>(p + i), j - i);
      i = j;
    } else {
      ++i;
    }
  }
  i = 0;
  while (i + 1 < n) {
    const uint16_t c = (uint16_t)((p[i] << 8) | p[i + 1]);
    if (c >= 0x20 && c < 0x7F) {
      std::string s;
      size_t j = i;
      while (j + 1 < n) {
        const uint16_t cc = (uint16_t)((p[j] << 8) | p[j + 1]);
        if (cc >= 0x20 && cc < 0x7F) {
          s.push_back((char)cc);
          j += 2;
        } else {
          break;
        }
      }
      if (s.size() >= static_cast<size_t>(minlen)) out.push_back(std::move(s));
      i = j;
    } else {
      ++i;
    }
  }
}

// ---------------------------------------------------------------------------
// Latest-sample snapshot (written only by the render thread, read by the
// remote server thread). Kept tiny + atomic so the read is lock-free.
// ---------------------------------------------------------------------------
struct Snapshot {
  std::atomic<int> state{kUnknown};
  std::atomic<int> item_count{0};
  std::atomic<int> prompt_seen{0};  // "Press A" prompt present
  std::atomic<int> menu_seen{0};    // main-menu option text present
  // Most recent distinct strings (for debugging / tuning the classifier).
  std::atomic<int> str_n{0};
  char str_buf[512];
};

inline Snapshot& snap() {
  static Snapshot* s = new Snapshot;  // leaked on purpose (stable address)
  return *s;
}

// Thread-safe accessors for the current game state (used by the remote
// control server's "game_state" command and any host-side poller).
inline int CurrentState() {
  return snap().state.load(std::memory_order_relaxed);
}
inline const char* CurrentStateName() { return StateName(CurrentState()); }

inline double now_s() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline FILE* logf() {
  static FILE* f = [] {
#ifdef _WIN32
    FILE* out = nullptr;
    if (::fopen_s(&out, "fable2_state_probe.log", "w") != 0) out = nullptr;
    return out;
#else
    return std::fopen("fable2_state_probe.log", "w");
#endif
  }();
  return f;
}

// Scan one candidate item object for strings, following heap pointers 1 level.
inline void scan_item(uint32_t item, std::vector<std::string>& out,
                      std::atomic<int>& guard) {
  if (!plausible(item)) return;
  if (guard.fetch_add(1) > 4000) return;
  uint8_t buf[256];
  if (!aread(item, buf, sizeof buf)) return;
  extract_strings(buf, sizeof buf, out);
  for (size_t k = 0; k + 4 <= sizeof buf; k += 4) {
    const uint32_t fld =
        (uint32_t)((buf[k] << 24) | (buf[k + 1] << 16) | (buf[k + 2] << 8) | buf[k + 3]);
    if (fld >= 0x80000000u && fld < 0x82000000u) {
      uint8_t sub[128];
      if (aread(fld, sub, sizeof sub)) extract_strings(sub, sizeof sub, out);
    }
  }
}

// Time-based throttle (the game can run well past 60fps, so per-frame would
// flood the log and the arena reads). sample(): keep the snapshot fresh every
// ~50ms; the log line is emitted every ~0.25s.
inline bool now_after_ms(int64_t& last, int64_t ms) {
  const int64_t t =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  if (t - last < ms) return false;
  last = t;
  return true;
}

inline int64_t t_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void do_sample();

// 1-second classifier timer thread. It runs independently of the game's render
// loop, so it keeps classifying even when the UI-text iter stops firing (e.g.
// during the main-menu movie, which renders a video with no UI text). The
// strong hooks (below) increment the call counters on the render thread; this
// thread reads those counters + the prompt heap and classifies once per second.
inline std::atomic<bool>& running() { static std::atomic<bool> r{false}; return r; }
inline void start() {
  static std::atomic<bool> started{false};
  bool exp = false;
  if (!started.compare_exchange_strong(exp, true)) return;
  running().store(true);
  std::thread([] {
    while (running().load()) {
      for (int i = 0; i < 10 && running().load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (running().load()) do_sample();
    }
  }).detach();
}
inline void stop() { running().store(false); }

// --- Rolling call-rate tracking for the discriminative functions. --------
// ConstTrue_Predicate_82C43198 is near-idle except in the main menu, where it
// runs at hundreds of thousands of calls/s (verified via the func trace).
// UITextPrompt_Render renders the "Press A" prompt.
inline std::atomic<uint64_t>& menu_total() { static std::atomic<uint64_t> c{0}; return c; }
inline std::atomic<uint64_t>& prompt_total() { static std::atomic<uint64_t> c{0}; return c; }
inline std::atomic<uint64_t>& title_total() { static std::atomic<uint64_t> c{0}; return c; }
inline std::atomic<uint64_t>& prompt_elem_total() { static std::atomic<uint64_t> c{0}; return c; }
inline void tick_menu() { menu_total().fetch_add(1, std::memory_order_relaxed); }
inline void tick_prompt() { prompt_total().fetch_add(1, std::memory_order_relaxed); }
inline void tick_title() { title_total().fetch_add(1, std::memory_order_relaxed); }
inline void tick_prompt_elem() { prompt_elem_total().fetch_add(1, std::memory_order_relaxed); }

// Scan the run-dependent heap region for the UTF-16BE "Press <a_img> to start"
// prompt string. Match on "to start" (00 74 00 6F 00 20 00 73 00 74 00 61 00 72
// 00 74). The string lives in the 0x426xxxxx region (stable region, run-
// dependent offset).
inline bool heap_has_prompt() {
  static const uint8_t pat[16] = {0, 0x74, 0, 0x6F, 0, 0x20, 0, 0x73, 0, 0x74, 0,
                                  0x61, 0, 0x72, 0, 0x74};
  uint8_t buf[65536];
  for (uint32_t off = 0x42640000; off < 0x42700000; off += 65536) {
    if (!aread(off, buf, sizeof buf)) continue;
    for (size_t k = 0; k + 16 <= sizeof buf; ++k)
      if (std::memcmp(buf + k, pat, 16) == 0) return true;
  }
  return false;
}

// Time in ms since the process (the static is initialised on first use, which
// is effectively process start since the probe is referenced early).
inline int64_t boot_ms() {
  static const int64_t start =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
             .count() -
         start;
}

// Classify the current state. Called once per second by the timer thread
// (see start/stop). Reads the render-thread call counters + the main UI text
// list + the prompt heap, updates the thread-safe snapshot, and logs to the
// in-game logger (REXSYS) on state change + to the file every sample.
inline void do_sample() {
  static bool first = true;
  static uint64_t last_menu = 0, last_title = 0, last_pet = 0;
  static int64_t last_t = 0;
  const int64_t t = t_ms();
  double menu_rate = 0.0, title_rate = 0.0, pe_rate = 0.0;
  if (first) {
    first = false;
    last_menu = menu_total().load(std::memory_order_relaxed);
    last_title = title_total().load(std::memory_order_relaxed);
    last_pet = prompt_elem_total().load(std::memory_order_relaxed);
    last_t = t;
  } else {
    const int64_t dt = (t - last_t > 0) ? (t - last_t) : 1;
    const uint64_t mt = menu_total().load(std::memory_order_relaxed);
    const uint64_t tt = title_total().load(std::memory_order_relaxed);
    const uint64_t pet = prompt_elem_total().load(std::memory_order_relaxed);
    menu_rate = (double)(mt - last_menu) * 1000.0 / (double)dt;
    title_rate = (double)(tt - last_title) * 1000.0 / (double)dt;
    pe_rate = (double)(pet - last_pet) * 1000.0 / (double)dt;
    last_menu = mt; last_title = tt; last_pet = pet; last_t = t;
  }

  // Before the guest arena is mapped (early boot) we can't read it; report
  // "unknown" so the caller knows the classifier has no data yet.
  const bool ready = page_ok(reinterpret_cast<const void*>(kArena + 0x42640000u));

  // Main UI text list (0x83334AA0). When the "Press A" prompt is on screen its
  // item object (vtable 0x8200A114) is the list's single entry; the item object
  // pointer lives at A+272 where A = *(*(list+4)). It changes when the prompt
  // is removed (e.g. the main-menu movie), so this is the "prompt visible" flag.
  uint32_t item_this = 0, item_ptr = 0, item_vtable = 0;
  if (ready) {
    const uint32_t wrapper = aread32(kList + 4);
    if (plausible(wrapper)) {
      const uint32_t A = aread32(wrapper);
      if (plausible(A + 264) && plausible(A + 272)) {
        const uint32_t B = aread32(A + 264);
        item_this = B + (A + 272);  // per recompiled 'add r31,r11,r10'
        if (plausible(item_this)) {
          item_ptr = aread32(item_this);
          if (plausible(item_ptr)) item_vtable = aread32(item_ptr);
        }
      }
    }
  }
  const bool prompt_alloc = ready && heap_has_prompt();
  // prompt_visible: the "Press A" prompt elements are being drawn this second
  // (pe_rate high). ~0 during the movie / empty list.
  const bool prompt_visible = pe_rate > 8.0;

  // One-time structural dump to nail the list layout for the prompt_visible
  // signal. Logs the main UI list + the prompt element-list (0x83334E20).
  if (ready) {
    static bool dumped = false;
    if (!dumped) {
      dumped = true;
      char buf[512];
      int o = 0;
      auto words = [&](const char* tag, uint32_t addr, int n) {
        o += std::snprintf(buf + o, sizeof(buf) - (size_t)o, "%s 0x%08X[", tag, addr);
        for (int w = 0; w < n && o < 480; ++w)
          o += std::snprintf(buf + o, sizeof(buf) - (size_t)o, "%s%08X",
                             w ? " " : "", aread32(addr + w * 4));
        o += std::snprintf(buf + o, sizeof(buf) - (size_t)o, "] ");
      };
      words("desc", kList, 12);
      const uint32_t wrapper = aread32(kList + 4);
      words("wrap", wrapper, 8);
      const uint32_t A = aread32(wrapper);
      words("A+248", A + 248, 14);
      REXSYS_INFO("[state] DUMP {}", std::string(buf));
      if (logging()) {
        FILE* f = logf();
        if (f) { std::fputs(buf, f); std::fputc('\n', f); std::fflush(f); }
      }
    }
  }

  // State machine (see plans/ai-remote-input-control.md):
  //   menu (ConstTrue ~40,000+/s vs <1,000/s baseline) -> MainMenu
  //   prompt item in the main list                     -> PressAScreen
  //   past the first prompt, no prompt/menu            -> MainMenuMovie
  //   before the prompt (early boot)                   -> PreMainMenu
  //   arena not mapped yet                             -> "?"
  static bool past_first_prompt = false;
  if (prompt_visible || prompt_alloc) past_first_prompt = true;
  // Menu = sustained high ConstTrue rate (real menu ~40,000-87,000/s). A single
  // 1s burst (e.g. a splash transition spike) must not trigger it, so require
  // the rate to be high for several consecutive samples.
  static int menu_run = 0;
  if (menu_rate > 10000.0) menu_run = (menu_run < 12) ? menu_run + 1 : 12;
  else menu_run = 0;
  // Real menu is a sustained ~44,000-87,000/s; the splash loader produces a
  // transient 216,000-1,600,000/s spike lasting only ~4 samples. Require a
  // longer sustained run so the loader spike is ignored.
  const bool menu = menu_run >= 5;
  int state;
  if (!ready) state = kUnknown;
  else if (menu) state = kMainMenu;
  else if (prompt_visible) state = kPressAScreen;
  else if (past_first_prompt) state = kMainMenuMovie;
  else state = kPreMainMenu;

  Snapshot& sn = snap();
  sn.prompt_seen.store((int)(prompt_visible ? 1 : 0), std::memory_order_relaxed);
  sn.menu_seen.store((int)(menu ? 1 : 0), std::memory_order_relaxed);
  sn.item_count.store((int)(title_rate + 0.5), std::memory_order_relaxed);
  sn.state.store(state, std::memory_order_relaxed);

  static const char* names[] = {"?", "PreMainMenu", "PressAScreen",
                                "MainMenuMovie", "MainMenu"};
  static int last_state = -1;
  const char* cur_name = names[state];

  // In-game logger (same channel as the remote control server): state changes
  // only, so the AI / player can see transitions without log spam.
  if (state != last_state) {
    const char* prev_name =
        (last_state >= 0 && last_state <= 4) ? names[last_state] : "?";
    last_state = state;
    REXSYS_INFO(
        "[state] boot={}ms: {} -> {} (menu={} title={} pv={} pa={} item_vt=0x{:08X})",
        (long long)boot_ms(), prev_name, cur_name, (int)menu_rate, (int)title_rate,
        prompt_visible ? 1 : 0, prompt_alloc ? 1 : 0, item_vtable);
  }

  // File log (FABLE2_STATE_PROBE=1): every sample, for classifier tuning.
  if (logging()) {
    FILE* f = logf();
    if (f) {
      char hdr[300];
      std::snprintf(hdr, sizeof hdr,
                    "boot=%lldms  %-13s  (menu=%.0f title=%.0f pe=%.0f pv=%d pa=%d "
                    "item_ptr=0x%08X item_vt=0x%08X past=%d)\n",
                    (long long)boot_ms(), cur_name, menu_rate, title_rate, pe_rate,
                    prompt_visible ? 1 : 0, prompt_alloc ? 1 : 0,
                    item_ptr, item_vtable, past_first_prompt ? 1 : 0);
      std::fputs(hdr, f);
      std::fflush(f);
    }
  }
}

}  // namespace fable2::stateprobe

// Strong overrides: count calls to the discriminative functions, then run the
// original (weak) body. These run on the render thread at high frequency, so
// the tick is a single relaxed atomic increment (no I/O, no allocation).
// Debug/remote builds only (FABLE2_REMOTE_CONTROL); release builds use the
// original weak bodies untouched.
#ifdef FABLE2_REMOTE_CONTROL
extern "C" void __imp__ConstTrue_Predicate_82C43198(PPCContext&, uint8_t*);
extern "C" void ConstTrue_Predicate_82C43198(PPCContext& ctx, uint8_t* base) {
  fable2::stateprobe::tick_menu();
  __imp__ConstTrue_Predicate_82C43198(ctx, base);
}
extern "C" void __imp__ToLower_SdkRuntime_82CA4288(PPCContext&, uint8_t*);
extern "C" void ToLower_SdkRuntime_82CA4288(PPCContext& ctx, uint8_t* base) {
  fable2::stateprobe::tick_title();
  __imp__ToLower_SdkRuntime_82CA4288(ctx, base);
}
// sub_82B458C0 = prompt element-list fetch/next (returns element, 0 = done).
// Count the non-zero returns: each drawn prompt element ticks once, so the
// rate is high while the "Press A" prompt is on screen and ~0 during the movie
// (empty element list) -- this is the prompt_visible signal.
extern "C" void __imp__sub_82B458C0(PPCContext&, uint8_t*);
extern "C" void sub_82B458C0(PPCContext& ctx, uint8_t* base) {
  const uint32_t in_list = ctx.r3.u32;
  __imp__sub_82B458C0(ctx, base);
  if (ctx.r3.u32 != 0 && in_list >= 0x83330000u && in_list < 0x83340000u)
    fable2::stateprobe::tick_prompt_elem();
}
#endif  // FABLE2_REMOTE_CONTROL
