# osgx aipython Knowledge

osgx builds only the `osgx` module. It ships two ways: as part of the
`OpenSceneGraph` pip wheel (via its `etc/osgx` submodule — most sessions will
hit this path, see Bootstrap below), or as a local CMake checkout next to a
sibling `OpenSceneGraph.py` checkout (see "Local dev checkout" below).

**This file covers only the osgx basics (build layout, bootstrap, module
shape). For detailed agent guidance on driving a live viewer/REPL session —
most osgx-specific gotchas already found (cursor tracking, depth readback,
osgx.Library bootstrap, headless rendering, build-dir/LD_LIBRARY_PATH
mechanics, etc.) — that knowledge lives in the `OpenSceneGraph` Python
project's own `aipython/00-index.md`, not here.** osgx ships as part of the
same `OpenSceneGraph` wheel (or, in a local checkout, builds alongside it —
see "Local dev checkout" below), so that project's docs are the shared
source of truth for aipython mechanics across both — duplicating them here
would only go stale.

`OpenSceneGraph` declares the `aipython.skills` entry point, so its docs are
reachable whether or not there's a local checkout on this machine:
`list_local_skills()` finds it if one is checked out at a sibling path on
`sys.path`/`root`; otherwise `list_package_skills()` finds it as an installed
package (`package="OpenSceneGraph"`). Either way, read its `00-index.md` via
`read_local_skill()`/`read_package_skill()` and follow its routing table. This
file stays short: just what's unique to osgx itself (its own build layout and
Python module shape) that OSG.py's docs have no way to know.

## Bootstrap

No source checkout is required. `osgx` builds into the same wheel as
`OpenSceneGraph` (its `etc/osgx` submodule — see "Local dev checkout" below),
so a plain install covers both plus the examples and the aipython MCP server:

```bash
pip install openscenegraph openscenegraph-examples aipython
```

```python
import osgx
from OpenSceneGraph import *   # osg, osgGA, osgViewer, osgDB, ...

osgx.build_info()  # {'version': '0.1.2', 'pybind': ..., 'compiler': 'GCC 13.3.0', ...}
```

`osgx` needs a live `osgx.Library` before using most of its API (shader
expansion, shader cache, BRDF LUTs, `PixelText`, `osgx.gltf.readNodeFile`) —
see OSG.py's `01-core.md` rule 13 for the full explanation; the short version
is `lib = osgx.initialize()`, kept referenced for the session.

Some OS-level dependencies (Dear ImGui for `osgx.imgui`, EGL/GBM for
`osgx.platform`'s `createEGLWindow`/`createGBMWindow`) may or may not have
been present on whatever machine built a given wheel — check with
`hasattr(osgx, "imgui")` rather than assuming either way.

## Local dev checkout

This repo (`osgx`) is itself only a CMake build, not something `pip install`
sees directly — it's the source of truth that OSG.py's `etc/osgx` submodule
and wheel are built from. If you're working from this checkout rather than a
pip install, its `BUILD-*/OpenSceneGraph` is a symlink into the matching
`BUILD-*/OpenSceneGraph` of a sibling `OpenSceneGraph.py` checkout (default
`${CMAKE_SOURCE_DIR}/../OpenSceneGraph.py`, override with
`-DOSGX_OPENSCENEGRAPH_PY_DIR`; osgx statically links that build's
`libOpenSceneGraph_python{d}.a` and reuses its headers), and OSG.py's build
dir symlinks back to osgx's `osgx.cpython-*.so`. So one `sys.path` entry of
*either* `BUILD-*` dir makes both importable, and both resolve to the same
two real binaries:

```python
import sys
sys.path.append("BUILD-g++-13.3.0-NOASAN")  # whichever build dir is current

import osgx
from OpenSceneGraph import *
```

This mirrors the repo-root `conftest.py` that pytest uses for
`test/osgx_*.py`. `conftest.py` also does
`os.putenv("OSG_THREADING", "SingleThreaded")` before importing — unconfirmed
whether that's still load-bearing after the 2026-07-22 `FrameAccumulator`
cull/draw race fix in `osgx::debug`, or just an inherited habit. Set it
anyway until someone verifies it's safe to drop. After changing OSG.py
utility code that osgx links statically, relink osgx after OSG.py's rebuild.

Two build dirs typically exist side by side here (e.g.
`BUILD-g++-13.3.0-NOASAN` and `BUILD-clang++-18.1.3-NOASAN`) — check
`osgx.cpython-*.so` mtimes if unsure which is current, and prefer the newer
one unless the task specifically concerns a compiler-specific issue. For
testing a fresh/uninstalled build specifically (installed libs otherwise win
on `LD_LIBRARY_PATH`), see OSG.py's `01-core.md` rule 15.

## Submodule surface

This is the one thing genuinely specific to osgx's own module shape — OSG.py's
docs have no visibility into it. Most of the Python API is flat on the `osgx`
module itself — `pbr`/`ibl`/`picking`/`shadow`/`gbuffer`/`aura`/`shapes`/
`gizmos`/`pixel_text` all collapsed out of their own C++ namespaces in the
2026-08-20 flatten, and their bindings followed. Only `osgx.debug`,
`osgx.imgui`, `osgx.platform`, and `osgx.gltf` are real submodules, matching
real opt-in CMake targets — `imgui`/`gltf` only exist in a given build if it
was configured with `OSGX_WITH_IMGUI`/`OSGX_BUILD_GLTF` on.

`osgDB.Registry` has no `addFileExtensionAlias` binding, and no plugin claims
`.glb` by default; load glTF/GLB with
`osgx.gltf.readNodeFile(path, StopEvent(), osgx.gltf.AsyncProgress())` (a
blocking call; wrap in `asyncio.to_thread` to keep frames pumping).

## No standalone viewer/session helper here

osgx has no `Viewer`/session bootstrap of its own — it's a layer of utility
classes (camera manipulators, picking, cursor tracking, the debug profiler,
ImGui panels, glTF + PBR/IBL, typed lights/gizmos) applied to plain
`osg`/`osgViewer` objects from the `OpenSceneGraph` module. Everything about
actually standing up and live-driving a session — tmux/kernel mechanics,
callback safety, `controls.input.locked`/`controls.window.title`, camera
manipulators, RTT/MRT, picking + ImGui interaction, cursor/unproject tracking,
PBR/IBL lighting, shader hot-swap, etc. — lives in the `OpenSceneGraph`
project's own `aipython/` docs; see the pointer at the top of this file for
how to reach them (local checkout or installed package). Add topic docs to
*this* directory only for knowledge specific to osgx's own classes that
genuinely has no home there, once a live session has actually surfaced
something worth writing down.
