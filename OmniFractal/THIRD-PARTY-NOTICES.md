# Third-Party Notices

OmniFractal incorporates or links against the components listed below. Each is
licensed by its own authors under its own terms, which govern that component
and are **not** restricted by the OmniFractal EULA (see `LICENSE` §9).

Reproducing these notices is a binding obligation under the MIT, BSD-3-Clause,
and Apache-2.0 licenses, not a courtesy. Any permitted redistribution of
OmniFractal must carry this file.

The canonical machine-readable inventory is `cmake/third-party.json`. This
document is regenerated from it by the `omf_third_party_notices` build target;
edit the JSON, not this file.

---

## Summary

| Component | Version | License | Linkage | Required by |
|---|---|---|---|---|
| [GLM](https://github.com/g-truc/glm) | 1.0.1 | MIT | header-only | core maths |
| [volk](https://github.com/zeux/volk) | 1.3.270 | MIT | header-only | Vulkan entry-point loading |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 3.1.0 | MIT | header-only | GPU allocation |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.3.x | Apache-2.0 | header-only | Vulkan API declarations |
| [sol2](https://github.com/ThePhD/sol2) | 3.3.0 | MIT | header-only | Lua binding |
| [LuaJIT](https://luajit.org/) | 2.1 | MIT | **dynamic** | scripting runtime |
| [tinyexr](https://github.com/syoyo/tinyexr) | 1.0.8 | BSD-3-Clause | header-only | OpenEXR I/O |
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | MIT | static | DEFLATE for legacy import |
| [OpenCASCADE](https://dev.opencascade.org/) | 7.8.x | **LGPL-2.1 + exception** | **dynamic only** | STEP export |
| [gRPC](https://grpc.io/) | 1.62.x | Apache-2.0 | dynamic | tile dispatcher |
| [Protobuf](https://protobuf.dev/) | 25.x | BSD-3-Clause | dynamic | gRPC transport |
| [DockPanelSuite](https://github.com/dockpanelsuite/dockpanelsuite) | 3.1.1 | MIT | managed | UI docking |
| [AvalonEdit](https://github.com/icsharpcode/AvalonEdit) | 6.3.0.90 | MIT | managed | GLSL/Lua editor |

Every component above is either permissive (MIT / BSD / Apache-2.0) or is
dynamically linked under LGPL. No component imposes copyleft obligations on
OmniFractal's own source.

---

## OpenCASCADE Technology — linkage is a licensing requirement

OCCT is **LGPL-2.1 with an additional exception**. LGPL §6 permits combining it
with proprietary software only if the user retains the ability to relink
against a modified version of the library.

Consequently, in this project:

- OCCT is **always dynamically linked**. `OMF_OCCT_STATIC` does not exist, and
  `CMakeLists.txt` fails configuration if a static OCCT is detected.
- OCCT is reached only through `core/src/StepExporter.cpp`, behind
  `OMF_WITH_OPENCASCADE`. Builds without STEP export do not link it at all.
- `LICENSE` §9.2 carries an explicit carve-out preserving the LGPL relink,
  modify, reverse-engineer, and redistribute rights, overriding §5 and §8 of
  the EULA for that component.
- OCCT source corresponding to the linked binaries is available from
  <https://dev.opencascade.org/>. On request, the Licensor will supply the exact
  version and build configuration used.

Statically linking OCCT would subject the whole of OmniFractal to LGPL §6's source
and relink obligations, which conflicts with the proprietary posture. This is
why it is enforced at configure time rather than left to convention.

---

## LuaJIT

Dynamically linked. Scripts execute in a sandbox with `io`, `os`, `package`,
`require`, `dofile`, `loadfile`, and `ffi` removed — see
`core/src/LuaEngine.cpp`. `ffi` in particular is removed because it grants
unrestricted access to the host process's address space and to arbitrary
`dlopen`, which would make any downloaded script equivalent to a native
executable.

---

## Relationship to Mandelbulb 3D

OmniFractal reads the `.m3p`, `.m3i`, `.m3a`, `.m3l`, `.m3c`, `.m3v`, `.m3f`,
`.d3f`, and `.dSO` file formats for interoperability. It is **not** derived from
Mandelbulb 3D, is not a version or successor of it, and contains no code from
it.

- **Mandelbulb 3D** (Jesse, Tglad, et al.) is LGPL-2.1 licensed Delphi/Pascal.
  No part of it was copied, translated, or transliterated. The readers in
  `core/src/formats/LegacyImport.cpp` were written from an understanding of the
  formats' byte layout.
- **File formats are not copyrightable subject matter.** Implementing a reader
  for interoperability is protected — see *Sega v. Accolade* (9th Cir. 1992),
  *Sony v. Connectix* (9th Cir. 2000), and Article 6 of EU Directive 2009/24/EC.
- **The mathematics is not copyrightable.** Mandelbox, Amazing Box, Tglad folds,
  spherical inversion, and the Hubbard–Douady distance estimate are published
  results, reimplemented here from their public descriptions.
- **No third-party assets ship with OmniFractal.** Parameter presets, formula files
  (`.m3f` / `.d3f` / `.dSO`), gradients, light maps, and background images are
  the creative work of their authors — DarkBeam, Tglad, Trafassel, msltoe, and
  many others — and are not ours to redistribute. Point OmniFractal at your own
  Mandelbulb 3D installation to use them.

---

## Full license texts

### MIT (GLM, volk, VMA, sol2, LuaJIT, miniz, DockPanelSuite, AvalonEdit)

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

Copyright holders, respectively: G-Truc Creation (GLM); Arseny Kapoulkine
(volk); Advanced Micro Devices, Inc. (VMA); Rapptz, ThePhD and contributors
(sol2); Mike Pall (LuaJIT); Rich Geldreich and Tenacious Software LLC (miniz);
Lex Li and contributors (DockPanelSuite); Daniel Grunwald and AlphaSierraPapa
(AvalonEdit).

### BSD-3-Clause (tinyexr, Protobuf)

> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice, this
>    list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
> 3. Neither the name of the copyright holder nor the names of its contributors
>    may be used to endorse or promote products derived from this software
>    without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
> DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
> FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
> DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
> SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
> CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
> OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
> OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Copyright (c) Syoyo Fujita and contributors (tinyexr); Google Inc. (Protobuf).

### Apache-2.0 (Vulkan-Headers, gRPC)

Full text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) The Khronos Group Inc. (Vulkan-Headers); The gRPC Authors (gRPC).
Both are used unmodified; no NOTICE-file modifications apply.

### LGPL-2.1 with OCCT exception (OpenCASCADE)

Full text ships with the OCCT distribution and is available at
<https://dev.opencascade.org/caf/occt_lgpl_license>. See the linkage section
above and `LICENSE` §9.2 for the rights preserved to you.
