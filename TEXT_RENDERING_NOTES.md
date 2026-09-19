# Fable 2 ReXGlue — Custom UI Text Rendering (Notes & Breakthrough)

Goal: intercept the UI text rendering and (1) append `DEADBEEF` to the
"Press A to start" prompt as a canary, then (2) replace the Xbox **A** button
with a keyboard **E** keycap. Must be done at the *rendering* level (not by
editing data files / localization), and must not freeze the game.

---

## 1. Build / Run

- Build: from project root `D:/projects/hacking/Windows/Fable 2 Rexglue`
  ```
  cmd //c "build.cmd fable_2"
  ```
  (In MSYS / Git Bash you MUST use `//c`, not `/c` — MSYS mangles a single
  `/c` into a path.)
- Executable: `out/build/win-amd64-debug/fable_2.exe`
- Launch: PowerShell `Start-Process` with env vars set in the SAME command,
  working directory = `out/build/win-amd64-debug/`. Logs are written to the
  exe dir (CWD).
- Decrypted guest image: `scratch/fable2_image_decrypted_expanded.bin`
  (23,166,976 bytes, image base `0x82000000`).
- Recompiled guest code: `generated/default/fable_2_recomp.{0..291}.cpp`
  (~306 MB, 293 files). Each function: `DEFINE_REX_FUNC(sub_XXXXXXXX) { ... }`.

### Env vars (probe toggles)
| Var | Effect |
|-----|--------|
| `FABLE2_TRACE_WINDOW=1` | Enable function-call tracing |
| `FABLE2_TRACE_DELAY=<s>` | Seconds after launch to start trace window |
| `FABLE2_TRACE_DUR=<s>` | Trace window duration |
| `FABLE2_TRACE_FILTER=<substr>` | Only log function names containing substr |
| `FABLE2_TEXT_PROBE=1` | Dump UI widget tree (text probe) |
| `FABLE2_HEAP_SCAN=1` | Background arena string scanner |
| `FABLE2_HEAP_SCAN_REWRITE=1` | Also rewrite found strings (DEADBEEF) |
| `FABLE2_HEAP_SCAN_TIMES=N` | Number of scans (default 6) |
| `FABLE2_TEXTOBJ_DUMP=1` | Dump current-text object + font (list dump) |
| `FABLE2_UI_TEXT_DUMP=1` | UI text list dump (not currently wired) |
| `FABLE2_DEADBEEF=1` | **Append " DEADBEEF" to every UI text string (the canary)** |
| `FABLE2_DEADBEEF_LOG=1` | Log DEADBEEF appends to `fable2_deadbeef.log` |
| `FABLE2_DEADBEEF_EVERY=1` | Also try the known title-prompt heap addresses |

---

## 2. Guest memory layout (CRITICAL)

- Host arena base = **`0x100000000`** (a full 4 GiB region, commit-on-fault).
- Mapping: `host_ptr = 0x100000000 + guest_addr`  (for `guest_addr < 0xE0000000`).
  (MMIO `>= 0xE0000000` adds an extra `0x100000000`.)
- **The guest (Xbox 360 / PowerPC 750) is BIG-ENDIAN.** The recompiler reads:
  ```c
  #define REX_LOAD_U32(x) __builtin_bswap32(*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
  ```
  So any 32-bit value you read from guest memory must be `bswap32`'d to get
  the guest's value. **Wide strings are UTF-16BE** (high byte first):
  `'P'` = `00 50`, `'a'` = `00 61`, space = `00 20`.
- Image (code+rodata) at guest `0x82000000`–`0x83620000` (~22 MB).
  `.text` `0x82000000`–`0x83438000`; data `>= 0x83438000`.
- There is a second populated region at guest **`0x92000000`+** (a copy/alias
  of image data — strings found here at `0x920Axxxx` mirror the image's
  `0x820Axxxx`). Localized UI strings also appear in **`0x920Cxxxx`**.
- **Heap** is in the **`0x4xxxxxxx`** region (below the image). Runtime
  constructed strings live here.

### Reading guest memory from C++ (safe pattern)
```c
static uint32_t rd32(uint32_t a) {
  uint32_t v = 0;
  __try { v = __builtin_bswap32(*(volatile uint32_t*)(0x100000000ull + a)); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return 0xCCCCCCCC; }
  return v;
}
```
For byte reads (strings) endianness doesn't matter; use `host = 0x100000000 + a`.
To avoid AV on the render thread, gate with `VirtualQuery` (MEM_COMMIT +
readable) before reading, or use `__try/__except`. Do heavy scanning on a
**background thread** (render-thread scanning freezes the game).

---

## 3. Hooking recompiled functions (CRITICAL PATTERN)

Each recompiled guest function has TWO symbols (see `generated/default/fable_2_pch.h`):
```c
#define DEFINE_REX_FUNC(name)                                                       \
  extern "C" void __imp__##name(PPCContext&, uint8_t*);                            \
  __attribute__((weak, noinline)) extern "C" void name(                            \
      PPCContext& __restrict ctx, uint8_t* base) {                                 \
    [[clang::musttail]] return __imp__##name(ctx, base);                           \
  }                                                                                \
  extern "C" void __imp__##name(...) { /* real recompiled body */ }
```
- `sub_XXX` = **weak** wrapper that tail-calls the real body.
- `__imp__sub_XXX` = **strong** real body.
- The dispatch table (`generated/default/fable_2_register.cpp`) uses
  `registrar->SetFunction(0x82C03FA8, UIText_FrameRender); ...` — it references
  the **weak wrapper**.

**To hook `sub_XXX` (intercepts both direct `bl` and dispatch-table calls):**
```c
extern "C" void __imp__sub_XXX(PPCContext& ctx, uint8_t* base); // original body
extern "C" void sub_XXX(PPCContext& ctx, uint8_t* base) {       // strong override
  /* ... my logic ... */
  __imp__sub_XXX(ctx, base);   // call the ORIGINAL (no recursion)
}
```
Do NOT capture `&sub_XXX` (that's your own strong symbol → recursion). Call
`__imp__sub_XXX` for the original.

### PPCContext (the emulated register file)
- GPRs: `ctx.rN.u32/.u64/.s32/.s64/.f32` (union). `ctx.r1` = stack pointer.
- FPRs: `ctx.fN.f64` (union with `.f32/.u32`...).
- `ctx.lr` = link register (`uint64_t`), `ctx.ctr`, `ctx.crN`.
- Guest memory: use `REX_LOAD_U32(addr)` / `REX_STORE_U32(addr, val)` etc.
  (these are macros in the PCH that take `base` from scope).

---

## 4. Font / UI-text rendering pipeline

- Font module code range: `0x82BF0000`–`0x82C2xxxx`.
  **These functions are now named in `fable_2_manifest.toml`** (see the
  "UI text rendering pipeline" block at the bottom of that file).
- **Per-frame UI text pass:**
  `sub_82BFD850` → **`UIText_FrameRender`** (0x82C03FA8) →
  **`UIText_FrameRenderIter`** (0x82190760, `list, -1`).
  - **`UIText_FrameRender`** (0x82C03FA8): `r11 = 0x83330000; r4 = -1;
    r3 = r11 + 19104 (=0x83334AA0); tail-call UIText_FrameRenderIter`.
  - **`UIText_FrameRenderIter`** (0x82190760) iterates a list at `0x83334AA0`
    and draws each item via **`UITextItem_Dispatch`** (0x82BFDB68, `bctrl`).
- **Text render chain (active during title AND loading screens):**
  **`UIText_RenderCurrent`** (0x82C0AD60) → **`UIText_RenderCurrentObject`**
  (0x82C0ACD0) → **`UIText_RenderSegment`** (0x82C0A8F8, ×3) →
  **`UIText_RenderElement`** (0x82C0A6A0, per-glyph/element draw).
  - **`UIText_RenderCurrent`** (0x82C0AD60): `r3 = *(0x83330000 + 19756 =
    0x83334D2C); UIText_RenderCurrentObject(r3)`. So `*(0x83334D2C)` = pointer
    to the "current text" object.
  - **`UIText_RenderElement`** (0x82C0A6A0, element `r31`): vtable at `+0`;
    pointer at `+16` (→ its vtable `+0`, vtable slot 9 `+36` is invoked); flag
    bytes at `+152/+157/+158/+159/+172`; state at `+92/+160/+168`.
- The "font manager" at `0x83330000` is **actually a VFS/asset table** (slots
  hold pointers to file-path objects like `Art\Environment\...`,
  `GUI_SCREEN_LOADING_TIPS`, `LoadingScreenData`), NOT a font registry.
  (This mislabel caused a lot of confusion — the .bft font registry is elsewhere.)
- The glyph-quad emitters `sub_82BFE948 → sub_82C00338 → sub_82C0F360 →
  sub_82C106A8 → sub_82C0CAF8` fire **0 times** during both the title and the
  loading screen ⇒ the text is pre-baked / drawn by `sub_82C0A6A0`, not by
  per-character glyph emission on the hot path.

### vcall thunks
- **`UITextItem_Dispatch`** (0x82BFDB68; fires ~3400×/screen):
  `r11 = *(r3+4); if (r11) bctrl r11`. Dispatches an object's "slot-4 method".
  During the title the targets seen were **`UIText_RenderCurrent`**
  (0x82C0AD60) and `sub_82C44CF0`.

---

## 5. THE BREAKTHROUGH — the localized prompt strings

The title prompt is a **live UTF-16BE string** in the heap, and the "A" button
is a **markup tag**, not a literal letter.

### Primary (runtime title prompt) — guest `0x426690F0`
Raw bytes (UTF-16BE):
```
00 50 00 72 00 65 00 73 00 73 00 20 00 3C 00 61 00 5F 00 69 00 6D 00 67 00 3E
00 20 00 74 00 6F 00 20 00 73 00 74 00 61 00 72 00 74 00 00
```
= **`Press <a_img> to start`**
- `<a_img>` is a placeholder tag rendered as the Xbox **A** button icon sprite.
- Appending `DEADBEEF` = make this `Press <a_img> to start DEADBEEF`.
- Swapping A→E = change how `<a_img>` renders (or retarget the tag) to a
  keyboard **E** keycap.

### Other "to start" copies (same heap region `0x4266xxxx`)
- `0x42668C2C`, `0x42668DAC`, `0x42668E0C`, `0x4266910C`, `0x4266922C`:
  `to start` (UTF-16BE) each followed by float data (e.g. `40 1C EE FF` =
  ~2.0f, `3F 8E 6A A0` = ~1.0f) — likely per-state/position copies of the same
  prompt (blink/alpha variants). The surrounding floats suggest a struct of
  { string, pos/scale }.

### Localization data region `0x920Cxxxx` (UTF-16BE)
- `0x920C2084`: **`Press A to confirm`**
- `0x920C20AC`: **`Press X to remove trophy`**
- `0x920CAD84`: `to start again :(`
- `0x920CB11A`: `starting level for '` / `More levels [...]`
- (These use literal `A`/`X` letters, unlike the title prompt which uses
  `<a_img>`.)

### Image strings `0x920Axxxx` (ASCII, mirror image `0x820Axxxx`)
- `0x920A0553`: `to start Xbox Live` (from "Failed to start Xbox Live...")
- `0x920A521F`: `to start QoS listener, error 0x%08x`
- `0x920ABDE6`: `PressA` / `UIContract` / `OnPressB` (UI contract names)
- `0x92040D7B`: `Press found, key code is %d`

### UI "PressA/PressB" contract (image, `0x920ABDE6`)
`PressA`, `OnPressB`, `FillWorldIcon`, `FillWorldIconButton` — the UI
contract that binds the A/B button prompts to icons.

---

## 6. Probe headers in `src/`

| Header | Hook (strong `sub_XXX`) | Purpose | Env |
|--------|--------------------------|---------|-----|
| `fable2_text_probe.h` | `sub_822A2948`, `UITextItem_Dispatch` | Dump UI widget tree + heap ptrs | `FABLE2_TEXT_PROBE=1` |
| `fable2_deadbeef.h` | (via `UITextItem_Dispatch` in text_probe) | **Append " DEADBEEF" to every UI text string** | `FABLE2_DEADBEEF=1` |
| `fable2_heap_scan.h` | `UIText_FrameRender` | Background arena string scan (+ optional rewrite) | `FABLE2_HEAP_SCAN=1` |
| `fable2_list_dump.h` | `UIText_FrameRender` | Dump current-text object + font (big-endian) | `FABLE2_TEXTOBJ_DUMP=1` |
| `fable2_func_trace.h` | (PCH macro redefinition) | Log every guest function entry | `FABLE2_TRACE_WINDOW=1` |
| `fable2_ui_text_dump.h` | `UIText_FrameRender` | UI text list dump (NOT wired into main.cpp) | `FABLE2_UI_TEXT_DUMP=1` |

`main.cpp` currently includes: `fable_2_app.h`, `fps_meter.h`,
`fable2_text_probe.h`, `fable2_heap_scan.h`, `keyboard_gamepad.h`.

> **Symbol collision note:** `fable2_heap_scan.h`, `fable2_list_dump.h`, and
> `fable2_ui_text_dump.h` each define a strong `UIText_FrameRender`. Only ONE may
> be included at a time (or consolidate into a single `UIText_FrameRender` that
> fans out).

### Func trace format
- `fable2_func_trace.log` (exe dir): one function name per line, in call order,
  run-length encoded (`sub_XXX x 4821`). Indirect `bctrl` targets are logged by
  the RESOLVED name (the callee's own `REX_FUNC_PROLOGUE`).
- To find what a thunk calls: look at the line **after** the thunk.

---

## 7. Key addresses (cheat sheet)

| Guest addr | Meaning |
|-----------|---------|
| `0x100000000` | Host arena base (add guest addr to get host ptr) |
| `0x82000000`–`0x83620000` | Image (code + rodata) |
| `0x82000000` | Image base (`.text` start) |
| `0x92000000`+ | Second image mapping (strings mirror `0x82xxxxxx`) |
| `0x920Cxxxx` | Localized UI strings (UTF-16BE) |
| `0x426690F0` | **`Press <a_img> to start`** (runtime title prompt, UTF-16BE) |
| `0x4266xxxx` | Heap region with prompt copies + float params |
| `0x83330000` | VFS/asset table base (NOT a font registry) |
| `0x83334AA0` | Per-frame UI text list object |
| `0x83334D2C` | Slot holding pointer to "current text" object |
| `0x83334A08` | Slot = `0x40102AF0` (asset/VFS table object, vtable `0x8200CAE8`) |

### Key functions (all named in `fable_2_manifest.toml`)
| Manifest name | Addr | Role |
|---------------|------|------|
| `UIText_FrameRender` | 0x82C03FA8 | Per-frame UI text pass entry (→ `UIText_FrameRenderIter`) |
| `UIText_FrameRenderIter` | 0x82190760 | Iterates UI text list @ `0x83334AA0`, draws items |
| `UITextItem_Dispatch` | 0x82BFDB68 | vcall thunk: `bctrl *(item+4)` (item's slot-4 render) |
| `UIText_RenderCurrent` | 0x82C0AD60 | Loads `*(0x83334D2C)`, calls `UIText_RenderCurrentObject` |
| `UIText_RenderCurrentObject` | 0x82C0ACD0 | Renders the "current text" object (font slot `0x83334A08`, visible flag `+152`) |
| `UIText_RenderSegment` | 0x82C0A8F8 | Iterates a segment's glyph linked-list (called ×3) |
| `UIText_RenderElement` | 0x82C0A6A0 | **Per-glyph/element draw** (the hot text draw) |
| `UITextItem_Render` | 0x82BFD9E8 | Per-item text draw; loads font slot[2] (`0x83334A08`) → `UIText_RenderWithFont` |
| `UIText_RenderWithFont` | 0x82C000F8 | Draws a string using a font object |
| `UIFont_LookupGlyph` | 0x82C106A8 | Binary search over the font glyph/text table |
| `UIFont_EmitGlyphQuad` | 0x82C0CAF8 | Emits one glyph quad (3x3 matrix in r28) |
| `UIFont_RenderGlyph` | 0x82BFE948 | Per-glyph draw (position float at +8) → quad emitter |
| `UIFont_Initialize` | 0x82BF7DF0 | Font/.bft setup (FNV-1 hash table, font name strings) |

---

## 8. Session 2 — detailed pipeline decode (structures, tag, what was tried)

### 8.1 The `<a_img>` tag (how the "A" button is rendered)
The prompt string is literally `Press <a_img> to start`. The `A` is a
**markup tag**, not a font glyph. The renderer expands `<a_img>` into an
Xbox-A icon sprite inline in the text.

- **Tag name table** in the image: a static array of tag names at guest
  **`0x820D7B50`** region — `s1, s2, s3, s4, a_img, b_img, c_img, d_img, …`
  (also referenced from `0x820B02D0`). This is the tag→icon mapping table.
- To swap A→E: either (a) retarget `a_img` to a keyboard-E keycap sprite, or
  (b) detect input device and swap the sprite the tag resolves to.

### 8.2 Object structures (decoded from runtime dumps)
- **Element** (e.g. `0x421B5CE0`): vtable `0x8200AA58`; `+16` = draw object
  ptr; flag bytes `[152]=2, [158]=0, [172]=0x00FEB60D`. The visible
  prompt element takes the **step-4 vertex-emit path** (`[158]==0`).
- **Draw object** (e.g. `0x4216D170`): vtable `0x8200AEA0`; `+0x10` = material
  (path `GLB_ShortGrass.mdl`); `+0xE4` = **glyph run** pointer (transient,
  built during draw, cleared after).
- **Glyph run** (`draw_obj+0xE4`): list of 16 `{record_ptr, x_offset}` pairs.
  `record_ptr` (word 0 of pair) → 48-byte glyph record at `0x4640Cxxx`;
  `x_offset` (word 1) is an integer in the **~0x34000–0x37000** (215k–224k)
  range.
- **Glyph node** (e.g. `0x4Cxxxxxx`): `+0x00` = x (integer, 50–144059),
  `+0x04` = 1999, `+0x0C` = 1.0f, `+0x14` = `0x40510C50`.
- **EmitGlyphQuad 16-byte struct** (from `UIFont_EmitGlyphQuad` 0x82C0CAF8):
  `+0x00` = x (r4), `+0x04` = (r6), `+0x08` = UV (a GPU shared-mem offset in the
  `0xF29xxxxx` region — NOT an atlas UV), `+0x0C` = scale (r5+0).
- **el+0x28** (e.g. `0x425DCB10`) is a **string object** (vtable `0x820F6198`,
  `+0x0C` ASCII, `+0x04` length), NOT a glyph node.
- Element `+0x44..+0x5C` is **transient** (sub-item pointers only present
during draw).

### 8.3 The per-frame vertex-emit chain (RenderElement step 4)
```
UIText_RenderElement (0x82C0A6A0)
  → sub_82C09018            // state builder (drawobj.vtable[13]=0x82C12D90) + anim counters; does NOT emit vertices
  → sub_82C09B50(global, el[136], el+40)
  → sub_82C0A230(global, el, el[152], 1)
  → sub_82C09870(el)        // if el[32]!=0: el.vtable[13] (0x82B4EEE0)  ← the per-element DRAW
```
- Element vtable `0x8200AA58`: `[9]=0x82C093F0, [13]=0x82B4EEE0 (draw), [14]=0x82C0A988`.
- Draw vtable `0x8200AEA0`: `[7]=0x82C12ED8, [9]=0x82C12F78, [13]=0x82C12D90, [14]=0x82C12EA0`.
- The real per-element draw is **`element.vtable[13]` = 0x82B4EEE0**, invoked
  via `sub_82C09870`. The per-frame x position is consumed here (via the glyph
  records / run) — its exact source is still being located.

### 8.4 What was tried (and did NOT move the visible text)
1. **Shift `UIFont_EmitGlyphQuad` r4 (layout x).** Ineffective — r4 is the
   *layout-time* x, not the *per-frame draw* x.
2. **Shift the glyph run's x_offsets** (`draw_obj+0xE4`, words 1,3,…; the
   ~0x34000–0x37000 integers), applied once per run in the
   `UIText_RenderElement` hook. **Ineffective** — the visible text did not move.
3. **Shift the per-glyph vertex x** (first float of `record+0x2C` vertex data,
   ~0x46xxxxxx / tens-of-thousands). Tried in `sub_82C09870` (too late) AND in
   `sub_82C09018` (right after the state builder). **Still ineffective.**

### 8.7 Full RenderElement draw chain (decoded, session 2b)
```
UIText_RenderElement (0x82C0A6A0)  [r31=element, r30=render ctx]
  1. if el[172]==0 && el[157]!=0:
       draw.vtable[9] (0x82C12F78)  → query; if nonzero, bail
       draw.vtable[14](draw, stack+80) (0x82C12EA0); compare el[160]; el[168]+=ctx; if el[168]>7000: el[172]=1
  2. sub_82C09018(el, ctx)            // "state builder" (draw.vtable[13]=0x82C12D90) + anim
  3. if el[158]!=0:  if el[172]!=0: el[92]=1; return
  4. else:
       if el[159]!=0: sub_82C0A590(el); return
       else:
         draw.vtable[7] (0x82C12ED8)  // QUERY only (returns 0/1 from el[212]/el[213]); NOT the draw
         if el[152]==3 && query!=0 && el[32]!=0 && *(el[32]+12)!=0:
             (*(el[32]+12)).vtable[2]  // (offset 8)
         sub_82C09B50(0x82000C08, el[136], el+40)   // BUMP ALLOCATOR (global pool at +64/+68/+72)
         if el[152]!=1: sub_82C0A230(0x82000C08, el, el[152], 1)  // vtable dispatch on el[152]
         sub_82C09870(el)  // if el[32]!=0: el.vtable[13] (0x82B4EEE0) — also just a query
```
- **KEY:** `element.vtable[13]` (0x82B4EEE0) and `draw.vtable[7]` (0x82C12ED8)
  are both **queries** (return 0/1), NOT the draw. `sub_82C09B50` is a **bump
  allocator**; `sub_82C0A230` is a **vtable dispatch** (on `el[152]`). The real
  vertex write is the `sub_82C0A230` dispatch target (or the
  `(*(el[32]+12)).vtable[2]` call) — the vertex data is filled *after*
  `sub_82C09018`, which is why shifting it in `sub_82C09018` was too early.
- The vertex data at `record+0x2C` (the 0x46xxxxxx floats) is NOT the value the
  GPU reads for the on-screen x (shifting it had no effect). The actual
  per-glyph screen position is written to the **GPU shared-memory** region
  (`0xF0000000–0xFFFFFFFF`) by the dispatch target, which has not yet been
  instrumented.

### 8.5 Current hooks / probe headers (session 2)
| Header | Hook(s) | Purpose | Env |
|--------|---------|---------|-----|
| `fable2_text_append.h` | `UIText_RenderSegment`, `sub_82C09018`, `UIFont_LookupGlyph`, `UIFont_EmitGlyphQuad`, `sub_82C55AD8`, `sub_82C52A80` | probes + (disabled) append | `FABLE2_TEXT_APPEND=1`, `FABLE2_TA_PROBE=1`, `FABLE2_TA_SHIFT=<n>` |
| `fable2_glyph_probe.h` | `UIText_RenderElement` | dumps the transient glyph run + **shifts run x_offsets** (`FABLE2_TA_SHIFT`) | `FABLE2_GLYPH_PROBE=1` |
| `fable2_font_probe.h` | (bg thread, via `UIText_RenderElement`) | scans heap for the UTF-16BE prompt, dumps glyph objects + vertex buffer | `FABLE2_FONT_PROBE=1` |
| `fable2_hotfuncs.h` | 25-fn counter | counts which text fns fire | `FABLE2_HOTFUNCS=1` |

- `FABLE2_TA_SHIFT=<n>`: the shift amount applied to the run x_offsets (0 = off).
- Confirmed: shifting does NOT freeze or crash the game (run `b4a74523d` clean).

### 8.6 Confirmed non-goals / dead ends
- `sub_82C55160 → sub_82C52A80 → sub_82C55C40/88 → sub_82C55AD8` is a
  **marquee/progress animator**, NOT the main text (do not target it).
- `sub_82A6EF40` (218K calls) is a LightingManager reset, not the quad emitter.
- The "uv" (`obj+0x08`) is a **GPU shared-memory offset** (`0xF0000000–0xFFFFFFFF`),
  all zeros at dump time — NOT a font-atlas UV.
- Fable 2 does **NOT** use XAPI graphics; the host backend is D3D12
  (`thirdparty/rexglue-sdk-src/src/graphics/d3d12/`).
- Glyph data (64B at `0x404DDxxx`) is **identical per glyph** except position
  words at `+0x20`/`+0x30`.

---

## 9. Next steps (updated, session 2)

**Goal refinement:** The user rejected the host-side ImGui overlay. The
result must (a) use the **game's own font**, (b) be **anchored to the text**
(at the end of the prompt), and (c) **only appear when the text is present**.
Current milestone: prove a *visible* change by shifting the prompt text block
right; then keep the original in place and draw a copy ("DEADBEEF") after it.

1. **Find where the per-frame draw reads the glyph x.** Shifting the layout x
   (`UIFont_EmitGlyphQuad` r4) AND the glyph-run x_offsets (`draw_obj+0xE4`)
   both did NOT move the text. So the per-frame draw (`element.vtable[13]` =
   `0x82B4EEE0`, via `sub_82C09870`) reads the x from somewhere else. Dump the
   48-byte glyph record at `0x4640Cxxx` (the `record_ptr` in the run) and the
   state-builder output (`drawobj.vtable[13]` = `0x82C12D90`) to locate it.
   Then shift that value to move the text.
2. **Canary (visible result):** once the x source is found, shift the prompt
   block right (tune `FABLE2_TA_SHIFT`) so it is clearly visible. Confirm with
   the user on-screen.
3. **Append copy:** keep the original text in place and draw a second copy at
   the shifted x. Emit the glyphs **twice** (original x + shifted x). Then
   change the second copy's text to literal `DEADBEEF` (resolve D/E/A/B/F
   through the font glyph table, `UIFont_LookupGlyph` binary search over
   `font+84`).
4. **Get the font object at runtime:** the font manager at `0x83334A08` is a
   runtime global (the image holds a string, not the pointer). Capture it from
   the glyph-record references or from `UIFont_LookupGlyph`'s r3.
5. **Final:** make `<a_img>` render as a keyboard **E** keycap (retarget the
   tag via the `0x820D7B50` tag table, or swap the sprite it resolves to).
6. **Auto-swap (later):** detect last input device (keyboard vs Xbox) and pick
   the E-keycap vs A-button rendering.

### Open questions
- Where exactly does the per-frame draw (`0x82B4EEE0`) read the glyph x from?
  (The glyph record at `0x4640Cxxx` is the leading candidate.)
- Is the glyph run (`draw_obj+0xE4`) rebuilt every frame, or persistent? (Its
  x_offsets are stable across frames, but shifting them had no visual effect —
  so either the draw ignores them, or it re-derives x from the records.)
- Is `0x426690F0` a stable address, or must the code search for the UTF-16BE
  pattern each time it needs to patch?
- How does `<a_img>` get expanded to a sprite at draw time (the tag→icon
  mapping at `0x820D7B50`)? This determines how to inject an "E" keycap.
