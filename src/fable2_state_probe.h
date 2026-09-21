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
// The guest address space is mapped linearly to the 0x100000000 arena and
// spans far more than 0x80000000-0x84000000 (heap scan showed committed
// regions at 0x42xxxxxx .. 0x92xxxxxx). A pointer is "plausible" if its arena
// page is committed + readable.
inline bool plausible(uint32_t a) {
  if (a < 0x00400000u || a >= 0xC0000000u) return false;
  return page_ok(reinterpret_cast<const void*>(kArena + a));
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

// --- Rolling call-rate tracking. ------------------------------------------
// prompt_elem_total counts the prompt/manager element-list fetches (sub_82B458C0
// non-zero returns). A non-negligible rate means the prompt OR the main menu is
// being drawn; ~0 means the movie (video, no UI elements).
inline std::atomic<uint64_t>& prompt_elem_total() { static std::atomic<uint64_t> c{0}; return c; }
inline void tick_prompt_elem() { prompt_elem_total().fetch_add(1, std::memory_order_relaxed); }
// A-press timestamp (ms since start). Latched on the rising edge of the A
// button in the final merged pad state (see observe_a_button, called from the
// sub_822B2D60 hook), so it works no matter which input source drives A. The
// press is brief (a few hundred ms), so we latch the timestamp and compare
// in-sample instead of relying on catching the button mid-press.
inline std::atomic<int64_t>& last_a_press_ms() { static std::atomic<int64_t> t{0}; return t; }
inline void record_a_press(int64_t ms) { last_a_press_ms().store(ms, std::memory_order_relaxed); }
// Current A-button state from the FINAL pad state (all drivers OR-merged:
// remote + keyboard + physical). Set by the sub_822B2D60 hook (the guest's
// XamInputGetState wrapper). The rising edge (0 -> 1) latches last_a_press_ms,
// so the A-press works no matter which input source drives the A button.
inline std::atomic<int>& a_button_state() { static std::atomic<int> b{0}; return b; }
// Reads the full 16-bit button mask from a final X_INPUT_STATE (guest address);
// the mask is be<uint16_t> at offset +4.
inline uint16_t read_a_button_mask(uint32_t state_ptr) {
  uint8_t buf[2] = {0, 0};
  if (!aread(state_ptr + 4, buf, 2)) return 0;
  return (uint16_t)((buf[0] << 8) | buf[1]);
}
// Records a rising A-press edge if the final pad state just gained the A button
// (X_INPUT_GAMEPAD_A = 0x1000). Called from the sub_822B2D60 hook on the guest
// thread; the edge is latched with its exact timestamp.
inline void observe_a_button(uint32_t state_ptr) {
  const uint16_t mask = read_a_button_mask(state_ptr);
  const int now = (mask & 0x1000u) ? 1 : 0;
  const int prev = a_button_state().load(std::memory_order_relaxed);
  a_button_state().store(now, std::memory_order_relaxed);
  if (now && !prev) record_a_press(t_ms());
}

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
  static uint64_t last_pet = 0;
  static int64_t last_t = 0;
  const int64_t t = t_ms();
  double pe_rate = 0.0;
  if (first) {
    first = false;
    last_pet = prompt_elem_total().load(std::memory_order_relaxed);
    last_t = t;
  } else {
    const int64_t dt = (t - last_t > 0) ? (t - last_t) : 1;
    const uint64_t pet = prompt_elem_total().load(std::memory_order_relaxed);
    pe_rate = (double)(pet - last_pet) * 1000.0 / (double)dt;
    last_pet = pet; last_t = t;
  }

  // Before the guest arena is mapped (early boot) we can't read it; report
  // "unknown" so the caller knows the classifier has no data yet.
  const bool ready = page_ok(reinterpret_cast<const void*>(kArena + 0x42640000u));
  const bool prompt_alloc = ready && heap_has_prompt();

  // State machine (see plans/ai-remote-input-control.md):
  //   ProgramOpens           -> PreMainMenu
  //   after time             -> PressAScreen
  //   PressAScreen + time    -> MainMenuMovie
  //   PressAScreen + press A -> MainMenu
  //   MainMenuMovie + time   -> PressAScreen
  //   MainMenuMovie + press A-> PressAScreen
  //   (undetermined          -> "?" (Unknown))
  //
  // Signals:
  //   prompt_alloc : the "to start" string is allocated (persists once seen).
  //   pe_rate : manager element-list fetch rate. >8 => the prompt or the menu
  //     is being drawn; ~0 => the movie (video, no UI elements). The prompt and
  //     the menu share the same element lists/rate, so they cannot be told
  //     apart visually -- the A-press (the state machine's input transition)
  //     distinguishes them. prev_showing captures whether the A-press landed on
  //     the prompt (PressAScreen -> MainMenu) or on the movie (-> PressAScreen).
  const bool showing = pe_rate > 8.0;
  const int64_t a_press_t = last_a_press_ms().load(std::memory_order_relaxed);
  static int64_t last_press_t = -1;
  static bool in_menu = false;
  static bool prev_showing = false;
  if (a_press_t > 0 && a_press_t != last_press_t) {
    last_press_t = a_press_t;
    in_menu = prev_showing;  // A on the prompt -> menu; A on the movie -> not
  }
  static bool past_first_prompt = false;
  if (prompt_alloc) past_first_prompt = true;
  int state;
  if (!ready) state = kUnknown;
  else if (!prompt_alloc) { state = kPreMainMenu; in_menu = false; }
  else if (!showing) { state = kMainMenuMovie; in_menu = false; }  // movie
  else state = in_menu ? kMainMenu : kPressAScreen;
  prev_showing = showing;

  Snapshot& sn = snap();
  sn.prompt_seen.store((int)(prompt_alloc ? 1 : 0), std::memory_order_relaxed);
  sn.menu_seen.store((int)(state == kMainMenu ? 1 : 0), std::memory_order_relaxed);
  sn.item_count.store((int)(pe_rate + 0.5), std::memory_order_relaxed);
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
        "[state] boot={}ms: {} -> {} (pe={} pa={})",
        (long long)boot_ms(), prev_name, cur_name, (int)pe_rate,
        prompt_alloc ? 1 : 0);
  }

  // File log (FABLE2_STATE_PROBE=1): every sample, for classifier tuning.
  if (logging()) {
    FILE* f = logf();
    if (f) {
      const int64_t a_age = (a_press_t > 0) ? (t - a_press_t) : -1;
      char hdr[220];
      std::snprintf(hdr, sizeof hdr,
                    "boot=%lldms  %-13s  (pe=%.0f pa=%d in_menu=%d a_age=%lld a_btn=%d)\n",
                    (long long)boot_ms(), cur_name, pe_rate,
                    prompt_alloc ? 1 : 0, in_menu ? 1 : 0, (long long)a_age,
                    fable2::stateprobe::a_button_state().load(std::memory_order_relaxed));
      std::fputs(hdr, f);
      std::fflush(f);
    }
  }
}

}  // namespace fable2::stateprobe

// Strong override of the element-list fetch (see below). Runs on the render
// thread at high frequency, so the tick is a single relaxed atomic increment
// (no I/O, no allocation). Debug/remote builds only (FABLE2_REMOTE_CONTROL);
// release builds use the original weak body untouched.
#ifdef FABLE2_REMOTE_CONTROL
// sub_82B458C0 = prompt/manager element-list fetch/next (returns element, 0 =
// done). Count the non-zero returns for lists in the 0x8333xxxx manager region:
// the rate is high (~15-17/s) while the "Press A" prompt OR the main menu is
// being drawn and ~0 during the movie (video, no UI elements).
extern "C" void __imp__ProcessAndProcessAndProcess573_82B458C0(PPCContext&, uint8_t*);
extern "C" void ProcessAndProcessAndProcess573_82B458C0(PPCContext& ctx, uint8_t* base) {
  const uint32_t in_list = ctx.r3.u32;
  __imp__ProcessAndProcessAndProcess573_82B458C0(ctx, base);
  if (ctx.r3.u32 != 0 && in_list >= 0x83330000u && in_list < 0x83340000u) {
    fable2::stateprobe::tick_prompt_elem();  // any manager element list
  }
}

// The guest's XamInputGetState wrapper: reads the FINAL merged pad state (all
// drivers OR-merged: remote + keyboard + physical). Capture the A button from
// it so the A-press is detected no matter which input source drives it. The
// X_INPUT_STATE pointer is r4 (the argument to this function); the original
// fills it, so read it after the call.
extern "C" void sub_822B2D60(PPCContext& ctx, uint8_t* base) {
  const uint32_t state_ptr = ctx.r4.u32;
  __imp__sub_822B2D60(ctx, base);
  fable2::stateprobe::observe_a_button(state_ptr);
}
#endif  // FABLE2_REMOTE_CONTROL
