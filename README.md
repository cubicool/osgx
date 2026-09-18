# osgx

<div align="center">

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
[![OpenSceneGraph](https://img.shields.io/badge/OpenSceneGraph-0080ff?style=for-the-badge)](https://github.com/openscenegraph/OpenSceneGraph)
[![Python](https://img.shields.io/badge/Python-pybind11-3776AB?style=for-the-badge&logo=python&logoColor=white)](https://pybind11.readthedocs.io/)

</div>

osgx is a small, compiled C++20 utility layer on top of OpenSceneGraph. It modernizes common OSG
idioms (concepts, ranges, spans, lambdas) and adds five optional, explicitly-included subsystems:

> [!NOTE]
> The `x` in `osgx` just means "eXtras"; it has nothing to do with `X11` or
> anything else Xorg-centric (`osgx::platform`'s X11/XRandr window helpers notwithstanding).

- `osgx::debug` — `GL_KHR_debug` integration (driver message callback, KHR debug-group annotations)
  plus a two-phase GPU/CPU per-drawable profiler.
- `osgx::imgui` — a Dear ImGui overlay (`Widget`/`Panel`) with pluggable sections and a built-in
  GPU-profiler, OSG-stats, and scene-texture browser.
- `osgx::platform` — X11/XRandr window helpers (`alwaysOnTop`, `listMonitors`, `moveWindow`), plus
  EGL- and GBM/DRM-backed `GraphicsWindow` factories for driving a window without GLX or X11 at
  all.
- `osgx::gltf` — a glTF 2.0 loader (`osgdb_gltf`), plus an optional `osgx::gltf::pbribl` adapter that
  renders it using the generic PBR/IBL/shadow facilities living flat in `osgx::`. Merged in from the
  formerly separate `osgGLTF` repo.
- `osgx::ktx2` — a KTX2 texture reader/writer built on vendored KTX-Software, merged in alongside
  `osgx::gltf` for the same reason (several `osgx::ibl` bake tools need KTX2 output).

All five live under `osgx/` and use the `osgx` namespace, but none of them is included by the
`osgx.hpp` umbrella header — pulling in `GL_KHR_debug`, Dear ImGui, X11/EGL/GBM, the glTF loader, or
KTX-Software is always an explicit opt-in (`#include "osgx/Debug.hpp"` / `#include "osgx/ImGui.hpp"`
/ `#include "osgx/Linux.hpp"` / `#include "osgx/gltf/Reader.hpp"` / `#include "osgx/ktx2/KTX2.hpp"`),
never something a consumer gets for free just by including the umbrella.

See [docs/](docs/) for the full per-subsystem reference, and the [Gallery](#gallery) below for a
look at what it renders.

# CMake

osgx can be consumed directly from its source tree or as an installed CMake package. In either
case, link the imported-style target and its OpenSceneGraph include and link requirements will be
propagated to your target.

To embed the source tree in another project:

```cmake
add_subdirectory(path/to/osgx EXCLUDE_FROM_ALL)
target_link_libraries(my_target PRIVATE osgx::osgx)
```

When embedded, examples, utilities, the Python module, and installation rules are disabled by
default. They can be enabled individually with `OSGX_BUILD_EXAMPLES`, `OSGX_BUILD_UTILS`,
`OSGX_BUILD_PYTHON`, and `OSGX_INSTALL`.

To install osgx and consume it as a package, first install an already-configured build tree:

```console
cmake --install BUILD --prefix /path/to/prefix
```

Then use the installed package from the consuming project:

```cmake
find_package(osgx CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE osgx::osgx)
```

Pass `-DCMAKE_PREFIX_PATH=/path/to/prefix` when configuring the consuming project if the chosen
prefix is not already in CMake's search path.

The same build tree can remove exactly the files recorded by its most recent install:

```console
cmake --build BUILD --target uninstall
```

This uses CMake's generated `install_manifest.txt`; keep the build tree until the installation is
no longer needed.

Use `osgx::core` for the low-level, header-only facilities (`Core.hpp`, `Array.hpp`,
`Callbacks.hpp`, `Shader.hpp`, `Visitors.hpp`, `Warnings.hpp`) or `osgx::osgx` for the complete
utility layer, including `osgx::debug`/`osgx::imgui`/`osgx::platform`. `osgx::osgx` is the compiled
`libosgx` library and includes `osgx::core`.

`osgx::gltf` (the glTF loader) and `osgx::ktx2` (the KTX2 reader/writer) are separate optional
static libraries, each gated behind its own `OSGX_BUILD_GLTF`/`OSGX_BUILD_KTX2` option (default ON
at the top level, OFF when embedded). They used to live in a separate `osgGLTF` repo; both were
folded directly into this tree since they were never meaningfully independent of `osgx::core`/
`osgx::osgx` to begin with. See [docs/GLTF.md](docs/GLTF.md).

# `osgx.hpp` — public headers

`osgx.hpp` is the umbrella header for the always-available utility layer. It keeps common setup
code shorter and adds modern range/span/lambda-friendly wrappers. Its public headers are organized
by concern:

- `osgx/Core.hpp` — smart-pointer helpers, timing, ring buffers, `findDataFile()`, and the
  `ObjectPath`/`vec_t`/literal utilities.
- `osgx/Visitors.hpp` — scene-graph visitors, event handlers, and `FilterNotifyHandler`.
- `osgx/Array.hpp` — `Array<BaseArray>` / `DrawElements<BaseElements>` wrappers.
- `osgx/Callbacks.hpp` — callback-group and lambda-callback adapters.
- `osgx/Picking.hpp` — object-ID picking cameras, readback, and hover/click handlers.
- `osgx/Manipulators.hpp` — `Ortho2DManipulator`, `OrbitAxisManipulator`, and `MultiCameraManipulator`.
- `osgx/CameraIntents.hpp` — `Viewpoint`, `FlyToCallback`, and `ShakeCallback`, driven by real
  `osgAnimation::Motion`/`CompositeMotion` (patrol legs, arrival latch, procedural shake).
- `osgx/Grid.hpp` — procedurally generated, antialiased grid overlay/ground-plane geometry.
- `osgx/Shapes.hpp` — `Polyhedron`-based primitive geometry (`Cube`, `Tetrahedron`, `Octahedron`,
  `Icosahedron`, `Dodecahedron`, `PentagonalTrapezohedron`) with explicit core-profile vertex
  attributes.
- `osgx/Shader.hpp` — generic, line-oriented GLSL library expansion.
- `osgx/PBR.hpp` — PBR BRDF snippets, typed direct lights (`LightSet`: directional/point/sphere/
  spot), and `OrbitLightRig`.
- `osgx/Gizmos.hpp` — `LightMarkers`/`LightGizmos` scene-space visualizations for `LightSet`
  lights (depth-tested markers for point/sphere/spot, plus a directional-only overlay camera).
- `osgx/Shadow.hpp` — single-light directional shadow mapping (`ShadowMap::create()`), a drop-in
  `DIRECT_LIGHTING_HOOK_SHADOWED` swap for `PBR.hpp`'s default direct-lighting hook.
- `osgx/IBL.hpp` — environment-map loading, BRDF-LUT baking (including the process-wide
  `SharedBRDFLUT::create()` cache), SH9/Lambertian diffuse irradiance, and cubemap readback helpers
  (`readCubeMapFaces()`, `BRDFLUTReadback`).
- `osgx/CaptureCubeMap.hpp` — `CaptureCubeMapScene`, the low-level frame-driven reflection-probe
  primitive (six ordered FBO cameras capturing a caller-owned scene into a radiance cubemap).
- `osgx/GGXPrefilter.hpp` — GPU GGX prefilter scene construction, rebaking, and readback.
- `osgx/LambertianBake.hpp` — frame-driven GPU Lambertian/diffuse cubemap baking and readback
  (`LambertianBakeScene`, `LambertianCubeReadback`).
- `osgx/GBuffer.hpp` — generic deferred G-buffer camera setup (`GBuffer::create()`), the primitive
  `osgx::gltf::pbribl`'s deferred forward/deferred split builds on.
- `osgx.hpp` — convenience umbrella that includes all of the above (but not `osgx::debug` or
  `osgx::imgui` — see [Documentation](#documentation) below).
- `osgx/Version.hpp` — `OSGX_VERSION_MAJOR`/`MINOR`/`PATCH` and the `OSGX_VERSION` string, generated
  from the CMake project version.

> [!NOTE]
> If you want to avoid having to `make install` before testing locally, you can
> use a command like the following: `export OSG_LIBRARY_PATH="$PWD/plugins/gltf:$PWD/plugins/ktx2${OSG_LIBRARY_PATH:+:$OSG_LIBRARY_PATH}"`
> This will ensure your local build's `osgdb_{ktx2,gltf}` files are used
> instead.

# Documentation

Per-subsystem deep dives live in [`docs/`](docs/):

- [`osgx` core](docs/CORE.md) — everything above that lives flat in `osgx::`: `Core`, `Visitors`,
  `Array`, `Callbacks`, `Picking`, `Manipulators`, `CameraIntents`, `Grid`, `Shapes`, `Shader`,
  `PBR`, `Gizmos`, `Shadow`, `IBL`, `GBuffer`, `Cursor`, and the cubemap-baking primitives it's
  built on.
- [`osgx::debug`](docs/DEBUG.md) — the three `GL_KHR_debug` systems, the two-phase GPU/CPU
  profiler, and `FrameByFrameViewer`.
- [`osgx::imgui`](docs/IMGUI.md) — the Dear ImGui overlay.
- [`osgx::platform`](docs/PLATFORM.md) — X11/XRandr and the EGL/GBM `GraphicsWindow` factories.
- [`osgx::gltf`](docs/GLTF.md) — the loader, its shader interface, the optional PBR/IBL renderer,
  and the environment-baking tool.

# Gallery

<table>
<tr>
<th align="center">Preview</th>
<th>Description</th>
</tr>

<tr>
<td align="center">

![osgx::gltf render compared with BabylonJS](ext/github/compare-babylonjs.png)

</td>
<td>

**glTF PBR/IBL parity**

`osgx::gltf::pbribl`'s output compared against BabylonJS, same model and environment lighting. The
goal isn't pixel-perfect matching, but a material response — reflections, roughness, HDR
environment lighting — that reads as a modern glTF renderer should. See
[docs/GLTF.md](docs/GLTF.md) for the full writeup.

</td>
</tr>

</table>

> More screenshots — one per example — are planned; this table is set up to grow.
