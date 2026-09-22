# Plan: AV/VirusTotal-clean builds (rexruntime.dll et al.)

## Problem

Every release build of `rexruntime.dll` (~9.9 MB, clang/lld/Ninja, unsigned) is a
**novel hash with an abnormal file profile**. AV vendors flag it via:

1. **No Authenticode signature** — the single strongest "untrusted" signal.
2. **Novel hash** — every build produces new bytes (PE timestamp, PDB GUID,
   embedded paths), so no hash ever accumulates population/reputation.
3. **Abnormal profile** — ~10,165 exports (`WINDOWS_EXPORT_ALL_SYMBOLS TRUE` in
   `thirdparty/rexglue-sdk-src/src/system/CMakeLists.txt:56`), no VS_VERSION_INFO
   resource (only the bare assembly manifest), large `.text`/`.rdata`, and in
   older builds extra HID.DLL/SetupAPI device-enumeration imports + OpenXR
   strings. That combination ("big unsigned DLL doing device enumeration,
   sockets, crypto, zero population") is the classic heuristic/ML false-positive
   shape (see e.g. Falcon Sandbox reports of rexruntime.dll: "suspicious 54/100,
   W64.Agent").

Side effect of the 10k-export setup: **any public function signature change
breaks the host exe at load time** (happened: `rex::BuildLogConfig` and
`rex::ui::Window::Create` signature changes → fable_2.exe missing 2 of 350
imports). Curating exports fixes that class of bug too.

## Status: no-cert mode (implemented)

Decision: **no code-signing certificate.** So the plan drops lever #1 and leans
on the cert-free levers. Without a signature the way to stay clean is to make
the file **stable + boring + populated**, so AV cloud/ML treats it as known
benign rather than novel/unknown.

What's in place now (see `tools/repro/README.md`):

- **Stable hash** — `tools/repro/normalize_pe.py` pins the PE timestamp, CodeView
  entry timestamp, RSDS PDB GUID, and RSDS PDB path to constants. Wired in as a
  `POST_BUILD` step on `fable_2` that runs over the staged
  `fable_2.exe` + all `rex*.dll` / `rexgpu-xenos*.dll`. A rebuild of the same
  source now yields the same file hash.
- **Machine-independent `__FILE__`** — `fable_2` / `fable_2_recomp` compile with
  `-ffile-prefix-map` / `-fdebug-prefix-map` so panic/assert paths are relative
  to the repo root (no `D:/projects/...` leakage).
- **Professional profile** — `fable_2.exe` gets a `VS_VERSION_INFO` resource
  (`tools/repro/fable2_version.rc`; CMake sets `CMAKE_RC_COMPILER=llvm-rc` in the
  Windows preset, and the `.rc` is only added when an RC compiler is present).
- **Cleaner imports already** — the current release `rexruntime.dll` no longer
  statically imports `HID.DLL`/extra SetupAPI (that was on the old build),
  which was the main "device enumeration" heuristic signal.

Still open / optional:
- **Export-table curation** — *lower priority than originally planned.* The
  ~10k exports are a real ABI (2,866 `__imp_` Xbox-kernel shims + 2,461 `rex::`
  API + VMA), not just bloat, so shrinking it risks the recompile. Do it only if
  it's worth the risk: export the exact consumer union as a `.def`.
- **Version resource on the DLLs** — rebuild the SDK from
  `thirdparty/rexglue-sdk-src` with a `VS_VERSION_INFO` on `rexruntime` /
  `rexgpu-xenos` (prebuilt SDK DLLs lack one).
- **Population** — ship the stable builds + pro-upload the hash to VirusTotal.

## Goals

1. Fresh builds scan clean on VirusTotal (target 0–2/72, no vendor > "HEUR").
2. Stable, pinned pre-sign hash per source revision (reproducible build).
3. Stable ABI contract between host exe and rexruntime.dll.

## Levers, ranked by impact

| # | Lever | Impact | Cost |
|---|-------|--------|------|
| 1 | Authenticode code signing (CA-issued OV cert + rfc3161 timestamp) | Largest. Valid sig removes the "unsigned" signal for nearly all engines | ~$200–400/yr cert + hours |
| 2 | Normal file profile: curated export table, VS_VERSION_INFO, consistent identity | Removes the heuristic shape | 1–2 days, free |
| 3 | Deterministic build → stable hash, accumulating reputation, verifiable releases | Medium | ~1 day, free |
| 4 | Ops: distribution/population, VT gate, FP submissions | Medium/ongoing | Ongoing |

No single lever guarantees 0/72 forever (vendor ML changes), but 1+2 is the
standard way shipped custom tooling stays clean, and 3+4 harden it.

---

## Phase 1 — Code signing (biggest win)

- Buy an **OV (organization) code-signing certificate** from a CA already in
  root stores (DigiCert, GlobalSign, Sectigo, Entrust). Self-signed does not
  help — engines require a trusted CA chain. EV is optional later (mainly helps
  SmartScreen/browser trust, not AV).
- Sign **all shipped artifacts**: `fable_2.exe`, `rexruntime.dll`,
  `rexgpu-xenos.dll`, and the `rexruntime-vulkan.dll` / `rexruntime-d3d12.dll`
  variants. A signed exe next to unsigned DLLs (or vice versa) keeps the folder
  looking untrusted.
- Always use an **rfc3161 timestamp** so signatures remain valid after cert
  renewal/expiration:
  ```
  signtool sign /fd SHA256 /sha1 <CERT_THUMBPRINT> ^
      /tr http://timestamp.digicert.com /td SHA256 <file>
  signtool verify /pa /v <file>
  ```
- CMake integration: add a `POST_BUILD` command on each Windows target (or one
  post-build step signing everything staged into `out/build/win-amd64-release/`).
  Gate behind a `REXGLUE_CODE_SIGNING_CERT` variable so local/dev builds skip it.
- Key custody: keep the private key on the build machine (or USB token); for CI
  later, use a secrets manager + `signtool` agent.
- Keep signing **identity consistent** across builds: same company name, same
  product naming — engines learn trust per-certificate over time.

## Phase 2 — Normal file profile

### 2A. Curated export table (replaces `WINDOWS_EXPORT_ALL_SYMBOLS`)

- In `thirdparty/rexglue-sdk-src/src/system/CMakeLists.txt`, drop
  `WINDOWS_EXPORT_ALL_SYMBOLS TRUE` and link with an explicit `.def` file
  (`set_target_properties(rexruntime PROPERTIES LINK_FLAGS "/DEF:rexruntime.def")`
  or `target_link_options`).
- Build the def list as the union of:
  1. All 350 symbols `fable_2.exe` imports from rexruntime.dll (extract with
     pefile — script already exists from the analysis).
  2. The SDK's public API (headers under `include/`) that host/plugins use.
  3. Exports consumed by `rexgpu-xenos.dll` and the test suite (audit with the
     same pefile one-liner before removing anything).
- Keep **old mangled names** for symbols hosts still link against (e.g.
  `?BuildLogConfig@rex@@...PEBD...`, `?Create@Window@ui@rex@...II@Z`) until the
  host is rebuilt against the new API — or change host + def in the same commit.
- Result: ~350–500 exports instead of 10,165 (normal for a runtime), and the
  `.def` becomes the **ABI contract**: future signature changes break at link
  time of the host, not at user load time.

### 2B. Version resource (VS_VERSION_INFO)

- Add a `version.rc` per binary with FileDescription ("ReXGlue Runtime"),
  ProductName, CompanyName, FileVersion/ProductVersion, LegalCopyright,
  OriginalFilename, InternalName.
- CMake compiles `.rc` automatically; with the clang toolchain make sure
  `CMAKE_RC_COMPILER` points at `llvm-rc` (ships with LLVM). Drive
  FileVersion from CMake (`REXGLUE_VERSION`) so it bumps per release.
- Feeding engines a professional, consistent identity (name, company, version)
  reduces "junk file" heuristics.

### 2C. Imports & general shape (minor, optional)

- Optionally load `HID.DLL` / SetupAPI device-enumeration dynamically
  (`LoadLibrary`) so they leave the import table; low value once signed.
- Keep the embedded manifest (already present). Do **not** pack/compress.
  Keep PDBs out of the shipped folder (already separate).

## Phase 3 — Deterministic (reproducible) builds

Goal: same source → same pre-sign bytes → stable hash → population +
integrity pinning.

Volatile bytes to neutralize (clang + lld-link + Ninja):

1. **PE TimeDateStamp** (header) — patch to a fixed constant in a post-build
   normalize step (lld-link has no stable flag for this).
2. **PDB CodeView entry** (debug directory in `.rdata`): random GUID + age per
   link. Fix GUID + age to constants; use a fixed PDB name/path
   (`/pdbs:rexruntime.pdb`) so the embedded path string is stable.
3. **Path leakage** in strings/asserts: pass
   `-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/src` (and same for the SDK source
   dir) so absolute build-machine paths never land in the binary.
4. **Normalize script** (Python, run post-build, pre-sign): set timestamp,
   rewrite CodeView GUID/age, optionally validate no machine-specific paths
   remain. Reuse the PE parser from the analysis.
5. **Verification**: `build twice from clean tree → identical sha256`
   (pre-sign). Add this as a release check.
6. Note: the rfc3161 timestamp blob makes the *signed* bytes differ per build;
   that's fine — AV cares about signature validity. Publish the stable
   pre-sign hash (e.g. in the release notes) for integrity checks.

Order of operations in the build pipeline:

```
compile/link → normalize (timestamp, PDB GUID, paths) → signtool sign (+timestamp) → signtool verify → (optional) VT check
```

## Phase 4 — Ops

- **Distribution/population**: ship the signed builds to real users; population
  is an input to ML engines. Consistent filename + signed identity compounds.
- **VT gate**: free VT API key (4 req/min); post-build, submit the new hash and
  warn if detections > 2 or any vendor says non-heuristic ("trojan" vs "HEUR").
- **False-positive submissions**: if a vendor persists, file an FP report with
  the vendor (Kaspersky/ESET/McAfee etc. all have portals) — include the
  signing cert details.
- Optionally pin the cert thumbprint in the host exe (self-verification) so
  users can spot tampered DLLs.

## Sequencing & effort

| Step | Work | Effort |
|------|------|--------|
| 2A def file + export audit | script the audit, hand-curate, wire into CMake, rebuild host | ~1 day |
| 2B version.rc | write .rc, wire CMake (llvm-rc, version var) | ~2 h |
| Phase 1 cert + signing | buy cert, signtool POST_BUILD hook for all artifacts, verify step | ~half day + cert lead time |
| Phase 3 normalize + repro | normalize script, flags, double-build verify | ~1 day |
| Phase 4 VT gate | small script + key | ~1 h |

Recommended order: **2A → 2B → Phase 1 → Phase 3 → Phase 4**.
After 2A+2B+Phase 1, fresh builds should scan ~0/72 even before determinism.

## Acceptance criteria

1. `signtool verify /pa` passes on every shipped artifact; certificate chain
   valid, rfc3161 timestamp present.
2. Two clean builds of the same tree produce identical pre-sign sha256.
3. Fresh upload of a brand-new build to VirusTotal: ≤ 2 detections, none
   naming a concrete malware family.
4. `fable_2.exe` links and loads against the new `rexruntime.dll` (def-based
   ABI preserved); changing an exported signature is a link-time error for the
   host, not a user-side load failure.
