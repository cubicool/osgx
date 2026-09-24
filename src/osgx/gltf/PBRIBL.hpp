#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Group>
#include <osg/Math>
#include <osg/Node>
#include <osg/Quat>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>
#include <osg/Uniform>
#include <osg/Vec3>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <string>
#include <string_view>
#include <vector>

#include "osgx/Environment.hpp"
#include "osgx/LambertianBake.hpp"
#include "osgx/Shadow.hpp"
#include "osgx/Shader.hpp"
#include "osgx/GBuffer.hpp"

// Forward declaration only - keeps tinygltf_json_c.h out of this public header's include list.
// Only PBRIBL.cpp, which implements decodeIBLEnvironments(), needs the real definition. This is
// the standalone JSON backend (tinygltf_json_c.h), not the glTF model library (tiny_gltf_v3.h) --
// the osgx_pbribl manifest this decodes was never real glTF scene data, just a JSON file shaped
// enough like one to reuse a JSON-with-extensions parser.
struct tg3json_value;

namespace osgx::gltf::pbribl {

// Registers the osgx::gltf shader catalog used by `#pragma osgx::gltf ...`. Registration is
// idempotent.
void registerShaderLibs();

// Registers the generic osgx PBR/IBL catalogs plus glTF's catalogs, then expands them together.
// Keeping registration and resolution in this component avoids cross-shared-library registry
// assumptions for Python and plugin consumers.
std::string resolveShaderLibs(std::string_view source);

// The osgx::Environment rotation that matches the Khronos glTF-Sample-Viewer for glTF content (the
// loader converts glTF's Y-up to Z-up). osgx::Environment's own default is the equirect's natural
// orientation; loadEnvironment() applies this, and HDR-built environments for glTF content set it
// themselves: `environment->setRotation(KHRONOS_ENVIRONMENT_ROTATION)`.
inline const osg::Quat KHRONOS_ENVIRONMENT_ROTATION(-osg::PI_2, osg::Vec3(0.0f, 0.0f, 1.0f));

// One `environments[]` entry decoded from an `osgx_pbribl` glTF extension block (see
// ~/dev/osgdebug/TODO.md section 2b for the manifest schema this mirrors). Pure data: no textures,
// no I/O. `uri` is exactly what the manifest declared - relative, resolving it (osgx::findDataFile
// et al.) and loading/baking the result is the caller's job.
struct IBLEnvironmentManifest {
	struct Resource {
		std::string uri;

		bool valid() const { return !uri.empty(); }
	};

	// Either a URI to a serialized LUT (for existing portable manifests), or the exact built-in
	// contract understood by this renderer. The latter uses osgx::SharedBRDFLUT::create(size).
	struct BRDFLUTResource: Resource {
		std::string builtin;
		int size = 1024;

		bool valid() const { return !uri.empty() || !builtin.empty(); }
	};

	Resource specular;
	Resource diffuse;
	BRDFLUTResource brdfLUT;
};

// Decodes every `environments[]` entry out of an `osgx_pbribl` extension block. `extensionValue`
// is whatever `tg3json_object_get(root, "osgx_pbribl")` returns when parsing a standalone
// manifest document (loadEnvironment(manifestPath)) via tinygltf_json_c.h - the only
// caller today. Note this is a different value type from a real glTF asset's own embedded
// extensions (tg3_value, from tiny_gltf_v3.h's model parser, see osgx::gltf::detail::tg3_value
// helpers in tg3_util.hpp) - there's currently no code path that decodes an osgx_pbribl block
// embedded in a real asset rather than a standalone manifest file.
std::vector<IBLEnvironmentManifest> decodeIBLEnvironments(const tg3json_value* extensionValue);

// Loads a pre-baked environment: the manifest's specular and diffuse KTX2 cubemaps plus either a
// serialized BRDF LUT or the shared built-in one, rotated by KHRONOS_ENVIRONMENT_ROTATION.
// `manifest`'s relative URIs resolve against `baseDir`. Returns null (and logs) on failure.
// A built-in LUT used for the first time in this process leaves a pass in getBakeRoot().
osg::ref_ptr<osgx::Environment> loadEnvironment(
	const IBLEnvironmentManifest& manifest,
	const std::string& baseDir
);

// Loads `manifestPath` - a standalone manifest or a real asset's own embedded osgx_pbribl block
// both work - and its first declared environment, resolving URIs against the manifest's directory.
osg::ref_ptr<osgx::Environment> loadEnvironment(const std::string& manifestPath);

struct PBRIBLScene {
	osg::ref_ptr<osg::Node> node;
	osg::ref_ptr<osg::Uniform> debugMode;
	osg::ref_ptr<osg::Uniform> disableNormalMap;
	osg::ref_ptr<osg::Uniform> disableRoughnessMap;
	osg::ref_ptr<osg::Uniform> disableSpecularAA;
	// The osgx::Environment passed to create() and attached to `node`.
	osg::ref_ptr<osgx::Environment> environment;

	bool valid() const;

	// Applies the glTF PBR/IBL renderer to a node, lit by `environment` (required; the caller owns
	// it, may share it between scenes, and adds its getBakeRoot() to the graph if non-null). Its
	// intensities and rotation stay live-tunable on the Environment itself. `shadowMap`,
	// when non-null, swaps in osgx::DIRECT_LIGHTING_HOOK_SHADOWED in place of
	// osgx::DIRECT_LIGHTING_HOOK_DEFAULT and wires its depth texture + shadow uniforms onto
	// `node`'s StateSet - the caller still owns building the ShadowMap itself (its light
	// direction has to match whatever the caller populates into osgx::LightSet at
	// ShadowMap::casterIndex) and adding `shadowMap->camera` to the scene graph; this only handles
	// the shader-side wiring. Default (nullptr) is unshadowed IBL+direct lighting, unchanged from
	// before this parameter existed.
	//
	// `hooks` (osgx::HookList, Shader.hpp) SUBSTITUTES this Program's default shader for any slot
	// it names, in place of the built-in - e.g. `{{osgx::Hook::Skinning,
	// shader::SKINNING_HOOK_LINEAR_BLEND-as-a-Shader}}` enables standard glTF joint-matrix
	// skinning (reading the exact JointIndices/JointWeights/joint-matrix SSBO wiring the loader
	// (Skin.cpp) already populates unconditionally for every skinned primitive), in place of the
	// default identity passthrough; `{{osgx::Hook::Tonemap, ...}}` substitutes a custom tone curve
	// for the built-in PBR Neutral one. This Program supports exactly the `Tonemap`/`Skinning`
	// slots - see applyHooks()'s own comment for the "exactly one definition, never both" hook
	// contract. Whole-Program scope, not per-primitive selection (see TODO.md).
	static PBRIBLScene create(
		osg::Node* node,
		osgx::Environment* environment,
		bool diagnostics=false,
		const osgx::ShadowMap* shadowMap=nullptr,
		const osgx::HookList& hooks={}
	);
};

// ================================================================================================
// Deferred split: PBRIBLGBuffer::create() + PBRIBLLightingScene::create(), the two-camera
// counterpart to PBRIBLScene::create()'s one-shader-does-everything shape above. Generalizes the
// G-buffer layout (gAlbedo/gNormal/gMaterial/gEmissive + depth) OpenSceneGraph.py's
// examples/pyosg-lighting/11-sketchfab.py hand-built and validated pixel-for-pixel against
// Sketchfab's own renderer for its deferred G-buffer + SSAO/bloom post-fx capstone - the
// lighting pass runs the SAME osgx_EvaluateEnvironment()/osgx_DirectLighting() logic
// PBRIBLScene::create()'s monolithic shader does, just reading G-buffer textures instead of
// interpolated varyings. This is an architectural split, not new shader math.
// ================================================================================================

// Populated deferred G-buffer: `gbuffer.camera` is the PRE_RENDER geometry pass (add it to the
// scene graph); the typed texture refs below are exactly `gbuffer.colorTextures[0..4]`, broken
// out by name for readability at the call site. `normalTexture`/`positionTexture` are VIEW-space
// (matching the main camera whose real view matrix PBRIBLLightingScene::create()'s fullscreen
// quad rotates world-space values from) - not world-space, unlike PBRIBLScene::create()'s
// eye-space-varyings-then-rotate-in-shader approach; the lighting pass performs that same
// rotation itself, once per pixel instead of once per vertex. `positionTexture` is real eye-space
// position written straight from the vertex shader - NOT reconstructed from depth + an inverse
// projection matrix in the lighting pass, which turns out to be fundamentally unreliable here
// (see PBRIBLLightingScene::create()'s own comment for why) - `depthTexture` still exists for
// real GL depth-testing during the geometry pass (and for a caller's own debug/visualize use), it
// just isn't what the lighting pass reconstructs position from.
struct PBRIBLGBuffer {
	osgx::GBuffer gbuffer;
	osg::ref_ptr<osg::Texture2D> albedoTexture;   // rgb = albedo, a = ambient occlusion
	osg::ref_ptr<osg::Texture2D> normalTexture;   // rgb = view-space shading normal (RGB16F)
	osg::ref_ptr<osg::Texture2D> materialTexture; // r = roughness, g = metallic
	osg::ref_ptr<osg::Texture2D> emissiveTexture; // rgb = emissive (HDR), a = alpha coverage
	osg::ref_ptr<osg::Texture2D> positionTexture; // rgb = view-space position (RGBA32F)
	osg::ref_ptr<osg::Texture2D> depthTexture;

	bool valid() const;

	// Writes material only - no lighting, not even emissive add (that's still stored, just not
	// combined with anything until the lighting pass). `node` becomes the geometry pass's child,
	// same as the `node` a caller would otherwise hand to PBRIBLScene::create() directly.
	static PBRIBLGBuffer create(osg::Node* node, int width, int height);
};

// Extra lighting-pass inputs, each an independent, optional seam rather than one monolithic
// flag blob:
// - `shadowMap` mirrors PBRIBLScene::create()'s own parameter exactly (nullptr = unshadowed).
// - `aoTexture`, if set, is multiplied into the ambient term. The lighting pass does NOT bake
//   SSAO itself - this stays a seam, not a parameter, since a caller may want a different
//   occlusion source entirely (a baked lightmap, a compute-shader technique, none at all).
//   `osgx::SSAO::create()` (GBuffer.hpp) is the standard hemisphere-kernel implementation, built
//   for exactly this seam and ported from 11-sketchfab.py's own hand-built one; feed its
//   `aoTexture` straight in here. A caller wanting different kernel/blur tradeoffs is still free
//   to build its own pass instead - this is a texture seam, not a required call.
// - `tonemap=false` leaves this pass's output as raw linear HDR (no tone curve, no gamma) - for
//   a caller chaining bloom/exposure/etc. passes after this one instead of ending the pipeline at
//   this call. It strips the CALL (via OSGX_PBRIBL_NO_TONEMAP) but still attaches a DEFINITION of
//   osgx_Tonemap() - the identity one, TONEMAP_HOOK_IDENTITY. Attaching no hook at all would make
//   the function's existence depend on a #define, and a define is not guaranteed to be present at
//   every compile: OSG's realize-time GLObjectsVisitor pre-compile pass sees an empty define
//   string and would link a call with no definition. See TONEMAP_HOOK_IDENTITY's comment in
//   PBR.hpp for the full mechanism.
//
//   Note these are two separate decisions on purpose, not one flag split in two: WHICH tone curve
//   runs is a hook swap (see `hooks`), while WHETHER this pass emits display-referred or
//   linear output is a property of the pass's output contract, and gates the gamma encode as well
//   as the curve. Do not "simplify" by folding the gamma into the hook - a custom hook author
//   would then have to remember to apply a transfer function too.
// - `hooks` (osgx::HookList, Shader.hpp) - this pass supports two slots:
//   - `osgx::Hook::Tonemap` is used as THE definition of osgx_Tonemap(), in place of either
//     built-in. This is the seam for a custom tone curve (ACES, a look LUT, a filmic response);
//     it takes precedence over `tonemap`, since supplying a curve means you want it to run.
//     Compose it the same way any other osgx shader is composed - a FRAGMENT osg::Shader whose
//     source defines osgx_Tonemap(vec3), optionally splicing library snippets in by name:
//
//         auto hook = new osg::Shader(osg::Shader::FRAGMENT, osgx::resolveShaderLibs(R"GLSL(
//             #version 460 core
//             #pragma osgx::pbr TONEMAP_ACES
//             vec3 osgx_Tonemap(vec3 color) { return osgx_TonemapACES(color); }
//         )GLSL"));
//
//         options.hooks = {{osgx::Hook::Tonemap, hook}};
//
//   - `osgx::Hook::DeferredLighting` REPLACES this pass's entire fragment `main()` - the "I know
//     what I'm doing, rewrite the whole pipeline" escape hatch, not a leaf-function swap. A custom
//     shader still gets `#pragma osgx::gltf DEFERRED_LIGHTING_INPUTS, GET_GBUFFER` for the
//     G-buffer sampler uniforms and the structured `osgx_GBuffer`/`osgx_GetGBuffer(uv)`
//     decode the built-in default itself uses, plus whatever other `#pragma`-registered snippets
//     it wants (`osgx::pbr`/`osgx::ibl`/`osgx::shadow`/`osgx::gltf`) - it's free to ignore
//     `osgx_DirectLighting()`/`osgx_Tonemap()` entirely (still harmlessly attached; GLSL doesn't
//     error on a defined-but-unused function). As with `Tonemap` above, the source MUST be passed
//     through `osgx::gltf::pbribl::resolveShaderLibs()` (not the generic `osgx::resolveShaderLibs()`
//     - this catalog is registered under the pbribl-specific one) before wrapping it in
//     `osg::Shader()`, or the `#pragma osgx::gltf ...` line is left un-expanded in the literal
//     source and the driver fails to compile it (confirmed live: every symbol the pragma was
//     supposed to declare - G-buffer samplers, `osgx_mainViewMatrix`, `fragColor` - comes back
//     "undefined variable", plus a syntax error on the un-expanded `#pragma` line itself):
//
//         auto hook = new osg::Shader(osg::Shader::FRAGMENT, resolveShaderLibs(R"GLSL(
//             #version 460 core
//             #pragma osgx::gltf DEFERRED_LIGHTING_INPUTS, GET_GBUFFER
//             void main() { ... }
//         )GLSL"));
//
//         options.hooks = {{osgx::Hook::DeferredLighting, hook}};
//
//     See `examples/osgx-gbuffer-custom.cpp` for a complete worked example.
//
//   Both are SUBSTITUTIONS, not additions alongside the built-in: GLSL permits exactly ONE body
//   per function (and exactly one `main()`), so a competing definition is a link error, not an
//   override. This pass ALWAYS attaches exactly one definition per slot (see
//   PBRIBLLightingScene::create()'s own comment for why never-zero matters); `hooks` chooses
//   WHICH, it does not add another.
struct PBRIBLLightingPassOptions {
	bool tonemap = true;
	// Custom osgx_Tonemap() definition via osgx::Hook::Tonemap; empty uses the built-in `tonemap`
	// selects. See the notes above - this REPLACES the built-in hook, it is not attached
	// alongside one.
	osgx::HookList hooks;
	const osgx::ShadowMap* shadowMap = nullptr;
	osg::Texture2D* aoTexture = nullptr;
	bool diagnostics = false;
};

struct PBRIBLLightingScene {
	osg::ref_ptr<osg::Node> node;
	// The osgx::Environment passed to create() and attached to the lighting pass; null if none.
	osg::ref_ptr<osgx::Environment> environment;
	// Updated by PBRIBLLightingScene::update() - see PBRIBLLightingScene::create()'s own comment for
	// why the fullscreen quad's own ABSOLUTE_RF camera can't supply these automatically. No
	// projection-matrix uniform here (a first version had one, for reconstructing position from
	// depth) - see PBRIBLGBuffer::positionTexture's comment for why that reconstruction was
	// dropped entirely rather than fixed.
	osg::ref_ptr<osg::Uniform> mainViewMatrix;
	osg::ref_ptr<osg::Uniform> mainViewMatrixInverse;

	bool valid() const;

	// A fullscreen-quad lighting pass reading `gbuffer`. `environment` is OPTIONAL - pass null
	// for a caller whose `options.hooks[osgx::Hook::DeferredLighting]` override doesn't need
	// environment lighting (a custom override CAN still pull it in via `#pragma osgx::environment
	// ...`, it just isn't required to), rather than forcing an HDR bake or KTX2 load purely to
	// populate textures nothing samples. `mainCamera` is the real, on-screen viewer camera this
	// pass rotates the G-buffer's view-space normal/position into world space with - the quad
	// itself is necessarily
	// ABSOLUTE_RF (an identity view/projection is what
	// makes it cover the screen in NDC), so OSG's automatic osg_ViewMatrix resolves to identity
	// here, not mainCamera's real matrices (the same PRE_RENDER-breaks-osg_ViewMatrix issue
	// 11-sketchfab.py hit and fixed with a manually-maintained view-matrix uniform). **Call
	// update() from a preDrawCallback on the FIRST PRE_RENDER camera in the scene graph (by
	// render order), NOT from mainCamera's own preDrawCallback and NOT from application code
	// after viewer.frame() returns** - every PRE_RENDER camera finishes drawing before
	// mainCamera's own preDrawCallback fires (confirmed against OSG 3.6.5's
	// RenderStage::draw()), and a plain post-frame() call is a full frame later than that; either
	// one hands this pass a stale matrix relative to what the geometry pass actually rendered
	// with, which shows up as position/lighting artifacts that get worse while the camera is
	// actively moving. This is exactly the bug class PBRIBLGBuffer::create()'s own
	// `positionTexture` field exists to avoid for the PROJECTION matrix (which nested cameras can
	// silently disagree about); the VIEW matrix genuinely is shared correctly across
	// RELATIVE_RF-nested cameras, so this uniform only needs to be *fresh*, not reconstructed a
	// different way.
	static PBRIBLLightingScene create(
		const PBRIBLGBuffer& gbuffer,
		osgx::Environment* environment,
		osg::Camera* mainCamera,
		const PBRIBLLightingPassOptions& options={}
	);

	// See create()'s own comment for the call-site timing this depends on
	// (a preDrawCallback on the first PRE_RENDER camera, never mainCamera's own callback or
	// post-frame() application code).
	void update(const osg::Camera* mainCamera);
};

}
