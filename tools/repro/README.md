# Reproducible / AV-clean Windows builds (no code-signing cert)

Goal: every release build of `fable_2.exe` + `rexruntime.dll` (+ plugins) should
scan clean on VirusTotal / AV cloud services **without** paying for a
code-signing certificate.

## Why an unsigned build gets flagged

AV vendors flag a file when it is (a) **novel** (a hash they've never seen),
(b) **unsigned**, and (c) **unusual in profile**. A large, freshly-rebuilt,
unsigned runtime DLL with no version info hits all three — and because a plain
rebuild changes the timestamp + PDB GUID, *every build is a brand-new novel
hash*, so the file never accumulates a benign reputation.

## The strategy (no cert)

1. **Stable hash** — make a rebuild of the same source produce the *same*
   bytes. Then the shipped hash is fixed, gets scanned many times, and AV
   cloud/ML systems learn it is benign. This is the single biggest cert-free
   lever.
2. **Normal, professional profile** — a well-formed version resource,
   consistent identity, no build-machine path leakage. Makes it look like a
   real product instead of an ad-hoc blob.
3. **Population** — actually ship it so it gets observed; optionally pro-upload
   the stable hash to VirusTotal to seed its baseline.

## What this does

### `normalize_pe.py` (the core)
Post-build step that pins the volatile bytes so the hash depends only on
(source + compiler + flags):

| Volatile field | Where | Pinned to |
|---|---|---|
| PE `TimeDateStamp` | `IMAGE_FILE_HEADER` | fixed constant |
| CodeView entry timestamp | debug directory | fixed constant |
| RSDS PDB GUID | CodeView blob | fixed constant |
| RSDS PDB path | CodeView blob | `<basename>.pdb` (relative) |

It is idempotent, size-preserving, and skips files that aren't present. The
`__FILE__` strings (panic/assert messages) are made machine-independent at
*compile* time with `-ffile-prefix-map`, so they don't need patching.

Run it manually on any PE:

```
python tools/repro/normalize_pe.py fable_2.exe rexruntime.dll [--stamp 0x6554B000]
```

### Build integration (`CMakeLists.txt`)
- `fable_2` (and `fable_2_recomp`) compile with
  `-ffile-prefix-map=<sourcedir>= -fdebug-prefix-map=...` so embedded paths are
  relative to the repo root.
- A `VS_VERSION_INFO` resource (`fable2_version.rc`) is added to `fable_2.exe`
  (skipped automatically if no `CMAKE_RC_COMPILER` is configured).
- A `POST_BUILD` command runs `normalize_pe.py` over the staged binaries
  (`fable_2.exe` + every `rex*.dll` / `rexgpu-xenos*.dll`) *after* staging, so
  each `build.cmd -release fable_2` leaves a stable-hash, profile-clean set of
  files in `out/build/win-amd64-release/`.

The DLLs (`rexruntime.dll`, `rexgpu-xenos.dll`) may be prebuilt from the SDK;
the normalizer works on their final bytes regardless of how they were built.

## Verifying reproducibility

```
build.cmd -r fable_2
sha256sum out/build/win-amd64-release/fable_2.exe out/build/win-amd64-release/rexruntime.dll
# touch a source or force a relink, rebuild:
build.cmd -r fable_2
sha256sum out/build/win-amd64-release/fable_2.exe out/build/win-amd64-release/rexruntime.dll
# -> identical hashes for identical source
```

## Further (optional) hardening

- **Version resource on the DLLs too** — rebuild the SDK from
  `thirdparty/rexglue-sdk-src` with a `VS_VERSION_INFO` on `rexruntime` /
  `rexgpu-xenos` (the prebuilt SDK DLLs don't have one).
- **Pro-upload** the stable hash to VirusTotal after each release.
- **Curate the export table** (the `__imp_*` kernel shims + `rex::` API are a
  real ABI, so this is optional) — see `plans/av-clean-builds.md`.
- **Code-sign later** if you ever want a hard trust signal: it layers on top of
  all of the above.
