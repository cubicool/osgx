# `osgx::gltf` — glTF 2.0 loader

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
Khronos parity, but not (yet) exporting state that matches the osgEarth shader pipeline. The
loader produces only generic osgx data - `osgx::Material`s, the tangent/joint vertex attributes of
`osgx/Skinning.hpp`, joint palettes - so it renders through osgx's generic renderers
(`osgx::PBRScene`, `osgx::PBRGBuffer`/`PBRLightingPass`) or any custom one.

Be sure to call `osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");` if you want to
support GLB loading through the same plugin registration path as `.gltf`.

## CMake

`OSGX_BUILD_GLTF` (default ON at the top level, OFF when embedded) gates the `osgx::gltf` static
library and the `osgdb_gltf` osgDB plugin. `osgx::gltf` is the loader plus the `osgx_sdf` and
`osgx_environment` manifest loaders (`osgx/gltf/SDF.hpp`, `osgx/gltf/Environment.hpp`); it links
`osgx::osgx` publicly and the vendored `ext/tinygltf` submodule. This is also what `osgdb_gltf`
links.

```cmake
find_package(osgx CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE osgx::gltf)
```

```cpp
#include <osgx/gltf/Reader.hpp>

osgx::gltf::Reader reader;
auto result = reader.read(path, isBinary, options);
```

`osgdb_gltf` and the Python module both link the same `osgx::gltf` target; neither recompiles the
reader implementation or instantiates its own copy of tinygltf/STB source.

## Shader Interface

`osgx::gltf` loads geometry and materials without imposing a particular renderer. Every glTF
material becomes a plain `osgx::Material` StateAttribute (`osgx/PBR.hpp`) - factors, alpha mode/
cutoff, emissive factor, and the four texture maps - so a custom renderer reads it exactly the way
it would read any hand-built `osgx::Material`: `#pragma osgx::pbr MATERIAL_INPUTS, GET_MATERIAL`
(plus `GET_SHADING_NORMAL`, `GET_EMISSIVE`, `GET_ALPHA` as needed). Nothing on the material side
is glTF-specific, and no sampler uniforms need setting - the GLSL declares its own texture units.

The tangent and skinning vertex attributes are generic too (`osgx/Skinning.hpp`, see
[CORE.md](CORE.md#osgxskinninghpp)): the loader writes tangents to `osgx::TANGENT_ATTRIBUTE` and
joint indices/weights to `osgx::JOINT_INDICES_ATTRIBUTE`/`osgx::JOINT_WEIGHTS_ATTRIBUTE`, and
attaches an `osgx::Skin` per glTF skin to each node that references it.

```cpp
auto program = new osg::Program();

// Fragment source uses `#pragma osgx::pbr MATERIAL_INPUTS, GET_MATERIAL`, resolved via
// osgx::resolveShaderLibs().
osgx::bindMeshAttributes(*program);

model->getOrCreateStateSet()->setAttributeAndModes(program);
```


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

- `type`: `"SDF"` (read `.r`) or `"MSDF"` (median of `.rgb`). `data` is an array, like `osgx_environment`'s
  `environments`, so one asset can carry several sheets.
- `texture`: `{"uri": ...}`, relative to the manifest. `{"index": n}` (a glTF `textures[]` entry) is
  reserved for embedding in a real asset and is not implemented; like `osgx_environment`, only a standalone
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

## Rendering glTF content

glTF-loaded geometry is plain `osgx::Material` geometry: render it with `osgx::PBRScene` (forward) or
`osgx::PBRGBuffer`/`osgx::PBRLightingPass` (deferred), see [CORE.md](CORE.md#osgxpbrscenehpp). The
Khronos glTF-Sample-Viewer setup is an `osgx_environment` manifest plus `PBRScene`:

```cpp
#include <osgx/PBRScene.hpp>
#include <osgx/gltf/Environment.hpp>

auto environment = osgx::gltf::loadEnvironment("papermill.gltf");
auto scene = osgx::PBRScene::create(model, {.environment = environment.get()});

if(environment && scene.valid()) {
	if(environment->getBakeRoot()) root->addChild(environment->getBakeRoot());
	root->addChild(scene.node);
}
```

`osgx/gltf/Environment.hpp` holds the glTF-side pieces:

- `loadEnvironment(manifestPath)` (or `(manifest, baseDir)`) — an `osgx::Environment` from an
  `osgx_environment` manifest: pre-baked specular/diffuse KTX2 cubemaps plus a serialized or
  built-in BRDF LUT, with no HDR decode or cubemap bake at runtime. Returns null on failure.
- `KHRONOS_ENVIRONMENT_ROTATION` — the `osgx::Environment` rotation matching the Khronos
  glTF-Sample-Viewer for glTF content. `loadEnvironment()` applies it; an HDR-built environment
  (`osgx::make_ref<osgx::Environment>(hdrImage)`) lighting glTF content sets it with
  `environment->setRotation(KHRONOS_ENVIRONMENT_ROTATION)`.
- `EnvironmentManifest`/`decodeEnvironments()` — the decoded manifest data.

Skinned models deform with `{.hooks = {{osgx::Hook::Skinning, new osg::Shader(osg::Shader::VERTEX,
osgx::resolveShaderLibs(osgx::SKINNING_HOOK_LINEAR_BLEND))}}}`. Python: `osgx.PBRScene`,
`osgx.PBRSceneOptions`, `osgx.gltf.loadEnvironment`, `osgx.gltf.KHRONOS_ENVIRONMENT_ROTATION`.
`utils/osgx-gltf-viewer` is the complete C++ consumer.

## PBR/IBL environment baking

`utils/osgx-environment` turns one HDR equirectangular image into a complete `osgx_environment`
bundle consumable by `utils/osgx-gltf-viewer --env`:

```bash
utils/osgx-environment input.hdr environments/studio
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

| Output | `osgx-environment` default | Khronos Sample Viewer reference default |
| --- | ---: | ---: |
| GGX specular cubemap base size (`--prefilter-size`) | 128 | 256 |
| GGX samples per texel (`--samples`) | 1024 | 1024 |
| Lambertian diffuse cubemap size (`--diffuse-cube-size`) | 256 | 256 |
| Lambertian samples per texel (`--diffuse-samples`) | 2048 | 2048 |
| BRDF LUT size (`--lut-size`) | 1024 | 1024 |
| BRDF LUT integration samples | 512 (fixed) | 512 |

`osgx-environment` emits the full specular mip chain down to 1×1. The Khronos reference environment
uses an eight-level GGX chain; this mainly affects the roughest lookup levels.
