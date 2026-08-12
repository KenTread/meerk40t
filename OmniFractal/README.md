# OmniFractal

A native 64-bit, cross-platform fractal renderer with a C++20 / Vulkan 1.3
compute core, LuaJIT scripting, CAD-grade mesh export, and a .NET 9 desktop
front end.

OmniFractal **reads** the Mandelbulb 3D file suite (`.m3p`, `.m3i`, `.m3a`, `.m3l`,
`.m3c`, `.m3v`, `.m3f`, `.d3f`, `.dSO`) and converts it to its own `.m4d`
container. It is an independent work — not a version of, successor to, or
derivative of that application. See [`LICENSE`](LICENSE) §10.

> **Status: architectural scaffold.** The ABI, container format, importers,
> precision model, and build system are implemented. The GPU compute path
> still falls back to the CPU reference renderer — see [Status](#status).

---

## What the 64-bit rewrite actually buys you

Two separate wins that are constantly conflated. They are kept separate in the
UI and the docs on purpose:

| | What it removes | What it unlocks |
|---|---|---|
| **64-bit address space** | the ~2–3 GB ceiling (~50–100 Mpx practical limit) | large renders |
| **fp64 arithmetic** | the precision cliff at depth | deep zoom |

Neither implies the other. And the memory wall **moves rather than vanishes**:
RGBA32F + Z + world normals + SSAO is ~36 bytes per pixel before DoF or motion
buffers, so a gigapixel frame is ~36 GB resident. The tile engine is therefore
mandatory and built into the render graph from commit one — it now serves
100 Gpx rather than 100 Mpx.

## What it does not promise

**Importing a legacy parameter file loads its parameters faithfully. It does
not reproduce the original image bit-for-bit, and cannot.**

The originating application evaluated in 80-bit x87, which rounds to a 64-bit
significand at every operation. Vulkan offers fp32 and fp64 and nothing
between. In a chaotic iterated map a last-bit difference becomes a visible
surface change within a few hundred iterations.

Computing *wider* does not help: IEEE binary128 rounds at 113 bits — a
different dynamical system, not a superset. Both converge toward a true
attractor that neither reaches.

What **is** reproducible is evaluation *semantics*, and that is where nearly
all the visible difference lives. Ranked by contribution:

1. **DE tightness and method selection.** One non-analytic slot downgraded the
   whole chain to the numeric 4-point estimate — the dominant source of
   overstepping, and of the soft character of much legacy imagery.
2. **Sampling** — raystep multiplier, stepwidth limiter, first-step-random,
   raystep-sub-DEstop.
3. **Iteration cap truncation** — detail ends at max-iterations long before it
   ends at the rounding boundary.
4. **Arithmetic precision** — a distant fourth, until deep zoom, where it
   abruptly becomes first.

So legacy profiles lock 1–3 and run plain **fp64 on the GPU at full speed**.
No 80-bit CPU path is built.

### Precision profiles

Rendering behaviour changed between legacy releases, so profiles are versioned
per release rather than lumped into one "legacy" mode:

| Profile | Use |
|---|---|
| `legacy-1.99.12` | closest match for widely circulated parameter sets |
| `legacy-1.99.35` | later release; tighter estimate on all-analytic chains |
| `modern` | per-slot analytic DE, sampling free to be re-tuned |

The profile is **required** at import — never defaulted. Importing under the
wrong semantics produces an image that differs from the author's in ways
nobody can then explain.

### Precision tiers

| Tier | Use | Cost |
|---|---|---|
| fp32 | navigator, preview | full rate |
| fp64 | default; all legacy profiles | ~1/64 on consumer GeForce, ~1/32 AMD consumer, 1/2 datacenter |
| double-double (~106 bit) | deep zoom beyond legacy's reach | ~10–20× fp64 |

No fp128 — Vulkan tops out at `VK_KHR_shader_float64`. Perturbation theory is
the other deep-zoom lever but remains immature for 3D DE fractals; there is a
seam for it, not an implementation.

## Parameter migration is not a toggle

Legacy values were tuned against legacy's flaws. An artist dropped raystep
multiplier to 0.1 to fight overstepping and capped iterations where detail
stopped mattering at their working resolution. Carry those forward unchanged
and the result is slower **and** worse-looking — tight DE makes 0.1 pure waste,
while a stale cap starves structure that is finally resolvable. The user
toggles back and concludes the upgrade is broken.

`omf_migrate_parameters` therefore:

- raises raystep multiplier in proportion to the DE tightening;
- **rescales DEstop against the new pixel width** (DEstop is defined relative
  to pixel size — re-rendering a 1080p set at 16K without rescaling changes
  detail, overstepping, and antialiasing character; arguably better, but not
  fidelity);
- lifts the iteration cap to match newly resolvable detail.

Both parameter sets are stored in the `.m4d`, so migration is **reversible**.
A low-resolution A/B preview is available before committing — on a fractal,
"better or merely different" is not judgeable from memory.

Upscaling offers two clearly labelled modes: **legacy-locked** (DEstop scales
with resolution, original look preserved) and **unlocked** (detail grows).

## Security

Every legacy file is untrusted input.

- All reads go through a bounds-checked `ByteReader`; no unchecked pointer
  arithmetic in the importers.
- Every length field is capped **before** it drives an allocation.
- Decompression is bounded twice — absolute cap and compression-ratio cap. The
  ratio cap is what actually stops a zip bomb.
- **`.m3f` / `.d3f` / `.dSO` contain raw machine code. It is never loaded,
  mapped, relocated, or executed.** The importer reads declarative metadata,
  maps the name against a known-formula registry, and **disables** unresolved
  slots — substituting a similar formula renders a confidently wrong image,
  which is worse than a visibly missing one.
- Lua runs sandboxed: no `io`, `os`, `package`, `require`, `load`, and
  critically no `ffi` (which would grant arbitrary `dlopen` and raw process
  memory).
- Every parser has a libFuzzer harness (`-DOMF_BUILD_FUZZERS=ON`, clang) run
  under ASan + UBSan.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Everything third-party is optional and gated behind `OMF_WITH_*`. The core
builds and renders with all of them off — that is the CI configuration.

| Option | Default | Notes |
|---|---|---|
| `OMF_WITH_VULKAN` | ON | volk + VMA |
| `OMF_WITH_LUA` | ON | LuaJIT via sol2 |
| `OMF_WITH_TINYEXR` | ON | multi-layer EXR |
| `OMF_WITH_MINIZ` | ON | DEFLATE; required for legacy import |
| `OMF_WITH_OPENCASCADE` | OFF | STEP export; **dynamic link enforced** |
| `OMF_BUILD_FUZZERS` | OFF | requires clang |

OpenCASCADE is LGPL-2.1. A **static** OCCT fails configuration deliberately —
static linking would void the relink right that makes LGPL compatible with a
proprietary work at all. There is no override flag.

The UI:

```bash
dotnet build ui/src/OmniFractal.App/OmniFractal.App.csproj -c Release
```

## Layout

```
core/include/omf/Renderer.h    the C ABI — the whole host contract
core/include/omf/M4DFormat.h   native container
core/include/omf/LegacyImport.h  hardened importers + threat model
core/src/PrecisionProfiles.cpp  profiles and migration
core/src/formats/LegacyImport.cpp
core/shaders/                   Vulkan compute
core/tests/fuzz/                parser harnesses
ui/src/OmniFractal.Interop/            [LibraryImport] boundary, blittable mirrors
ui/src/OmniFractal.Formats/            managed import surface
ui/src/OmniFractal.App/                WinForms shell
```

Every ABI struct is asserted for **size and offset** on both sides —
`Renderer.cpp` static_asserts, `NativeStructs.cs` re-checks at startup. A
layout drift throws a readable exception instead of corrupting render settings.

## Status

Implemented: C ABI, `.m4d` container, hardened legacy importers, precision
profiles and migration, CPU reference raymarcher, marching-tetrahedra iso
extraction with watertight STL, STEP via OCCT, multi-layer EXR, sandboxed Lua,
managed interop, fuzz harnesses, build system.

Not yet wired: the Vulkan dispatch path (`extract_gpu` and the raymarch
pipeline fall through to the CPU reference implementation), persistent-thread
kernels with ray compaction, double-double tier, gRPC tile dispatcher, and most
of the UI panels. The GPU shaders are written and compile to SPIR-V; the host
side that binds and dispatches them is the next piece.

## License

Proprietary — source-available for portfolio review and evaluation. See
[`LICENSE`](LICENSE) and [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

No third-party creative assets ship here. Parameter presets, formula files,
gradients, light maps, and background images belong to their authors — point
OmniFractal at your own installation to use them.
