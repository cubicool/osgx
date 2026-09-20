# `osgx::gltf` — glTF 2.0 loader + PBR/IBL adapter

Basic glTF 2.0 mesh/texture support for OpenSceneGraph. As of the last full Khronos sample sweep,
works for all of [the Khronos samples](https://github.com/KhronosGroup/glTF-Sample-Models).

![osgx::gltf render compared with BabylonJS](../ext/github/compare-babylonjs.png)

The screenshot compares this renderer's PBR/IBL output against BabylonJS using the same model and
environment lighting. The goal is not pixel-perfect matching, but matching the material response
closely enough that reflections, roughness, and HDR environment lighting behave like a modern glTF
renderer should.

This code **started** as a patched version of the
[osgEarth](https://github.com/gwaldron/osgearth/tree/master/src/osgEarthDrivers/gltf) reference
implementation, but has since evolved into something entirely different — more features and
Khronos parity, but not (yet) exporting state that matches the osgEarth shader pipeline. It defines
both a "contract" (for custom renderers, via `osgx/gltf/Shader.hpp`) and a "ready-to-use" set of
helper functions (`osgx::gltf::pbribl`).

`osgx::gltf` and its optional `osgx::gltf::pbribl` adapter, along with `osgx::ktx2` (the KTX2
reader/writer), were merged in from the formerly separate `osgGLTF` repo: both already depended on
`osgx::osgx`, so keeping them in a separate repo bought nothing but cross-repo build/version
friction.

Be sure to call `osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");` if you want to
support GLB loading through the same plugin registration path as `.gltf`.

## CMake

`OSGX_BUILD_GLTF` (default ON at the top level, OFF when embedded) gates two static-library
targets, plus the `osgdb_gltf` osgDB plugin:

- `osgx::gltf` — the loader itself, plus the small `osgx_sdf` tile-set loader
  (`osgx/gltf/SDF.hpp`). Links `osgx::osgx` publicly and the vendored `ext/tinygltf` submodule; the
  SDF loader needs `osgx::SDF`, so it lives here rather than in its own target. This is also what
  `osgdb_gltf` links.
- `osgx::gltf_pbribl` — the optional PBR/IBL adapter (`osgx/gltf/PBRIBL.hpp`). Links `osgx::gltf` plus the
  full `osgx::osgx` PBR/IBL layer. Only an explicit consumer pays for this.

```cmake
find_package(osgx CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE osgx::gltf)       # loader only
target_link_libraries(my_viewer PRIVATE osgx::gltf_pbribl)   # + PBR/IBL renderer
```

```cpp
#include <osgx/gltf/Reader.hpp>

osgx::gltf::Reader reader;
auto result = reader.read(path, isBinary, options);
```

`osgdb_gltf` and the Python module both link the same `osgx::gltf` target; neither recompiles the
reader implementation or instantiates its own copy of tinygltf/STB source.

## Shader Interface

`osgx::gltf` loads geometry and materials without imposing a particular renderer. The public
`osgx/gltf/Shader.hpp` header defines the attribute locations, buffer bindings, texture units,
uniform names, and canonical GLSL material declaration populated by the loader.

Custom renderers can use the GLSL declaration directly and apply the matching program setup:

```cpp
#include <osgx/gltf/Shader.hpp>

auto program = new osg::Program();
auto stateSet = model->getOrCreateStateSet();

// Add shaders using osgx::gltf::shader::MATERIAL_INPUTS as part of the fragment source.
osgx::gltf::shader::configureProgram(*program);
osgx::gltf::shader::configureStateSet(*stateSet);

stateSet->setAttributeAndModes(program);
```

`configureProgram()` maps the tangent and skinning inputs to the locations used by the loader.
`configureStateSet()` maps the material samplers to the loader's base-color, normal, ORM, and
emissive texture units. These helpers are optional; the named constants in the same header can be
used when an application needs different program or StateSet ownership.

The Python bindings expose the same constants, GLSL source, and helpers under `osgx.gltf.shader`.


## `osgx_sdf` - baked distance-field tiles (`osgx/gltf/SDF.hpp`)

Part of `osgx::gltf` (no extra target). A glTF-shaped manifest carrying a root `osgx_sdf` extension
describes one or more baked SDF/MSDF atlases; `osgx::gltf::sdf::TileSet::load(path)` turns a `data[]`
entry into ONE shared `osg::Texture2D` plus named tiles, and `TileSet::attribute(name)` hands back an
`osgx::SDF` per tile (see `docs/CORE.md`, "Baked distance fields"). osgx only ever consumes distance
fields - whoever baked them is irrelevant (slughorn's `bin/slughorn sdf x.slug -o a.png` writes
`a.png` + `a.gltf` in exactly this schema).

```json
{
  "asset": {"version": "2.0", "generator": "slughorn"},
  "extensionsUsed": ["osgx_sdf"],
  "extensions": {"osgx_sdf": {"data": [{
    "type": "MSDF",
    "texture": {"uri": "atlas.png"},
    "tiles": {"circle": {"rect": [100, 10, 96, 88], "pixelRange": 16.0,
                         "range": 0.1, "texelsPerEm": 80.0, "emOrigin": [-0.1, -0.1]}}
  }]}}
}
```

- `type`: `"SDF"` (read `.r`) or `"MSDF"` (median of `.rgb`). `data` is an array, like `osgx_pbribl`'s
  `environments`, so one asset can carry several sheets.
- `texture`: `{"uri": ...}`, relative to the manifest. `{"index": n}` (a glTF `textures[]` entry) is
  reserved for embedding in a real asset and is not implemented; like `osgx_pbribl`, only a standalone
  manifest is decoded today.
- `tiles[name].rect` is `[x, y, width, height]` in PIXELS with the origin at the TOP-left of the image
  (glTF's own convention); the image must be stored upright. The loader derives OSG texture-space UVs
  itself (`TileSet::uvRect(name)`, V = 0 at the bottom), so a manifest never has to get a Y flip right.
- `pixelRange` is the total distance range in TEXELS (msdfgen's `-pxrange`). Required, with `type`,
  `texture` and `rect`.
- `range`/`texelsPerEm`/`emOrigin` are an optional em-space frame for effects that work in shape units;
  a generic renderer ignores them.

Bad tiles (malformed, or a rect outside the image) are skipped with a warning; the set is invalid only
when the file, the extension, the image, or every tile is unusable. Python: `osgx.gltf.sdf.TileSet`.

## Optional PBR/IBL Renderer

`osgx::gltf::pbribl` applies `osgx::gltf`'s material interface using the generic PBR/IBL/shadow
facilities living flat in `osgx::` (see [CORE.md](CORE.md) — those used to be separate `osgx::pbr`/
`osgx::ibl` namespaces, flattened 2026-08-20). The loader target remains shader-agnostic;
applications opt into this renderer explicitly via `osgx::gltf_pbribl` (see CMake above):

```cpp
#include <osgx/gltf/PBRIBL.hpp>

auto environment = osgx::gltf::pbribl::PBRIBLEnvironment::load("papermill.gltf");
auto scene = osgx::gltf::pbribl::PBRIBLScene::create(model, environment);

if(environment.valid() && scene.valid()) {
	if(environment.root) root->addChild(environment.root);
	root->addChild(scene.node);
}
```

`PBRIBLEnvironment` has two loading paths, kept as distinct verbs on purpose (disk load vs. dynamic
HDR bake are genuinely different operations): `PBRIBLEnvironment::load(manifest, baseDir)` /
`::load(manifestPath)` for pre-baked KTX2 resources (zero HDR decode or cubemap bake at runtime),
and `PBRIBLEnvironment::prepare(hdrPath, lutSize=1024)` for a fully dynamic bake — `envMap` is a
valid, bindable texture immediately, but its contents only become correct once
`specularBakeRoot`'s passes have actually run a few frames.

`PBRIBLScene::create()` also takes an optional `const osgx::ShadowMap*` — when non-null, it swaps
in `osgx::DIRECT_LIGHTING_HOOK_SHADOWED` in place of the default unshadowed hook and wires the
shadow depth texture/uniforms onto the node's StateSet; the caller still owns building the
`ShadowMap` itself and adding `shadowMap->camera` to the scene graph (see [CORE.md's Shadow
section](CORE.md#osgxshadowhpp)). An optional `const osgx::HookList& hooks` (see [CORE.md's
`Shader.hpp` section](CORE.md#osgxshaderhpp)) substitutes this Program's built-in shader for either
of the two slots it supports: `osgx::Hook::Skinning` enables standard glTF joint-matrix skinning
(`shader::SKINNING_HOOK_LINEAR_BLEND`) in place of the default identity passthrough, and
`osgx::Hook::Tonemap` substitutes a custom tone curve —

```cpp
auto scene = osgx::gltf::pbribl::PBRIBLScene::create(model, environment, 1.0f, 1.0f, false, nullptr, {
	{osgx::Hook::Skinning, new osg::Shader(osg::Shader::VERTEX,
		osgx::resolveShaderLibs(osgx::gltf::shader::SKINNING_HOOK_LINEAR_BLEND))}
});
```

The material GLSL helpers are registered under `#pragma osgx::gltf ...`, and their canonical
uniform/attribute names use the `osgx_gltf_*` prefix (for example, `osgx_gltf_Material` and
`osgx_gltf_textures`). Their canonical material declaration comes directly from
`osgx/gltf/Shader.hpp`. Python exposes the same API under
`osgx.gltf.pbribl`. `utils/osgx-gltf-viewer` is the corresponding complete C++ consumer.

### Deferred split

`PBRIBLGBuffer::create(node, width, height)` + `PBRIBLLightingScene::create(gbuffer, environment,
mainCamera, ...)` are a two-camera counterpart to `PBRIBLScene::create()`'s one-shader-does-
everything shape: a geometry pass writing material-only G-buffer textures (built on
[`osgx::GBuffer`](CORE.md#osgxgbufferhpp)), then a fullscreen-quad lighting pass running the same
`osgx_EvaluateIBL()`/`osgx_DirectLighting()` logic against those textures instead of interpolated
varyings. `PBRIBLLightingPassOptions` carries the lighting pass's independent, optional inputs —
`shadowMap` (same contract as `PBRIBLScene::create()`'s own parameter), `aoTexture` (an externally-
baked occlusion result, multiplied into the ambient term — this pass does not bake it itself;
[`osgx::SSAO::create()`](CORE.md#osgxgbufferhpp) is the standard implementation to feed this seam),
`tonemap`/`hooks` (whether/how this pass tone-maps its own output, vs. leaving it linear HDR for a
caller chaining further passes — `hooks` supports only `osgx::Hook::Tonemap` here, no vertex stage
to skin), and `colorTexture`/`renderOrderNum` (retarget the pass to an offscreen texture instead of
the backbuffer).

**Call `PBRIBLLightingScene::update(mainCamera)` from a `preDrawCallback` on the first `PRE_RENDER`
camera in the scene graph** (by render order) — not from `mainCamera`'s own `preDrawCallback`, and
not from application code after `viewer.frame()` returns. Every `PRE_RENDER` camera finishes
drawing before `mainCamera`'s own `preDrawCallback` fires, so either of those alternatives hands the
lighting pass a stale view matrix relative to what the geometry pass actually rendered with —
visible as position/lighting artifacts that worsen while the camera moves.

See `examples/osgx-gbuffer.cpp` for the full wiring, including the shadow camera plugged into the
split (depth-only, so it sits alongside the geometry pass rather than inside it) and a per-channel
G-buffer visualizer (press `0`-`5`).

## PBR/IBL environment baking

`utils/osgx-pbribl` turns one HDR equirectangular image into a complete
`osgx_pbribl` environment bundle consumable by `utils/osgx-gltf-viewer --env`:

```bash
utils/osgx-pbribl input.hdr environments/studio
utils/osgx-gltf-viewer --env environments/studio.gltf model.gltf
```

The command writes `studio-specular.ktx2`, `studio-diffuse.ktx2`, and `studio.gltf`. The manifest
refers to its two KTX2 files by basename, so keep them beside the generated `.gltf` file (or move
the whole bundle together).

The specular and diffuse cubemaps are derived from the HDR. The manifest identifies the built-in
split-sum GGX BRDF LUT and its size instead of writing an HDR-independent LUT file; the renderer
caches and bakes that LUT once per process. Existing manifests that provide a BRDF-LUT `uri` remain
supported.

Optional quality controls are `--prefilter-size`, `--samples`, `--diffuse-cube-size`,
`--diffuse-samples`, and `--lut-size`.

| Output | `osgx-pbribl` default | Khronos Sample Viewer reference default |
| --- | ---: | ---: |
| GGX specular cubemap base size (`--prefilter-size`) | 128 | 256 |
| GGX samples per texel (`--samples`) | 1024 | 1024 |
| Lambertian diffuse cubemap size (`--diffuse-cube-size`) | 256 | 256 |
| Lambertian samples per texel (`--diffuse-samples`) | 2048 | 2048 |
| BRDF LUT size (`--lut-size`) | 1024 | 1024 |
| BRDF LUT integration samples | 512 (fixed) | 512 |

`osgx-pbribl` emits the full specular mip chain down to 1×1. The Khronos reference environment
uses an eight-level GGX chain; this mainly affects the roughest lookup levels.
