#pragma once

#include "osgx/Environment.hpp"
#include "osgx/GBuffer.hpp"
#include "osgx/Shader.hpp"
#include "osgx/Shadow.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Node>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

namespace osgx {

// ================================================================================================
// Deferred PBR: PBRGBuffer::create() renders osgx::Material geometry into a G-buffer (material
// only, no lighting), and PBRLightingPass::create() shades it in a fullscreen pass with the same
// osgx_EvaluateEnvironment()/osgx_DirectLighting() logic a forward PBR shader runs, reading
// G-buffer textures instead of interpolated varyings.
//
// The "osgx::gbuffer" catalog (registered by osgx::Library) is the G-buffer read contract a
// custom lighting shader (osgx::Hook::DeferredLighting) uses:
//   DEFERRED_LIGHTING_INPUTS - `in vec2 vUV`, the five G-buffer samplers (gAlbedo, gNormal,
//                              gMaterial, gEmissive, gPosition), the osgx_mainViewMatrix/
//                              osgx_mainViewMatrixInverse uniforms PBRLightingPass::update()
//                              maintains, and `out vec4 fragColor`.
//   GET_GBUFFER              - struct osgx_GBuffer {albedo, ao, normal, roughness, metallic,
//                              emissive, alphaCoverage, position} and osgx_GetGBuffer(uv), the
//                              structured decode of PBRGBuffer's layout. Requires
//                              DEFERRED_LIGHTING_INPUTS.
// `normal` and `position` are VIEW space; rotate them with osgx_mainViewMatrixInverse.
//
// The built-in lighting shader, and any custom one that declares them via `#pragma
// import_defines`, sees these StateSet defines:
//   OSGX_PBR_AO          - PBRLightingPassOptions::aoTexture is set; `sampler2D aoTex` holds it.
//   OSGX_PBR_NO_TONEMAP  - PBRLightingPassOptions::tonemap is false and no Tonemap hook is given.
//   OSGX_PBR_DIAGNOSTICS - PBRLightingPassOptions::diagnostics is true.
//   OSGX_PBR_ENVIRONMENT - PBRLightingPassOptions::environment is set; without it the built-in
//                          shader's environment term is zero.
// ================================================================================================

// The G-buffer: `gbuffer.camera` is the PRE_RENDER geometry pass (add it to the scene graph); the
// typed texture refs are `gbuffer.colorTextures[0..4]` broken out by name. `normalTexture` and
// `positionTexture` are VIEW space. `positionTexture` is eye-space position written straight from
// the vertex shader, not reconstructed from depth: each nested PRE_RENDER camera clamps its own
// private copy of the projection during cull and never writes it back, so a projection matrix
// read off the main camera afterwards does not reliably match the one the geometry pass used.
// `depthTexture` still serves depth testing during the geometry pass.
struct PBRGBuffer {
	osgx::GBuffer gbuffer;
	osg::ref_ptr<osg::Texture2D> albedoTexture;   // rgb = albedo, a = ambient occlusion
	osg::ref_ptr<osg::Texture2D> normalTexture;   // rgb = view-space shading normal (RGB16F)
	osg::ref_ptr<osg::Texture2D> materialTexture; // r = roughness, g = metallic
	osg::ref_ptr<osg::Texture2D> emissiveTexture; // rgb = emissive (HDR), a = alpha coverage
	osg::ref_ptr<osg::Texture2D> positionTexture; // rgb = view-space position (RGBA32F)
	osg::ref_ptr<osg::Texture2D> depthTexture;

	bool valid() const;

	// Writes material only - no lighting, not even the emissive add (emissive is stored for the
	// lighting pass). `node` becomes the geometry pass's child. `hooks` may substitute the
	// Hook::Skinning shader (e.g. SKINNING_HOOK_LINEAR_BLEND); the default is the identity.
	static PBRGBuffer create(osg::Node* node, int width, int height, const HookList& hooks={});
};

// Lighting-pass inputs, each independent and optional:
// - `environment`: attached to the pass (same contract as PBRSceneOptions::environment).
// - `tonemap=false` leaves the output as linear HDR (no tone curve, no gamma), for a caller
//   chaining bloom/exposure passes after this one. It strips the CALL (OSGX_PBR_NO_TONEMAP) but
//   still attaches a DEFINITION of osgx_Tonemap() - the identity one, TONEMAP_HOOK_IDENTITY - since
//   OSG's realize-time pre-compile sees no defines and would otherwise link a call with no
//   definition (see TONEMAP_HOOK_IDENTITY in PBR.hpp). WHICH curve runs is a hook swap (`hooks`);
//   WHETHER the pass emits display-referred output is this flag, which also gates the gamma
//   encode, so a custom curve never has to apply a transfer function itself.
// - `hooks` (HookList, Shader.hpp) substitutes a built-in shader object per slot:
//   - Hook::Tonemap: THE definition of osgx_Tonemap(), taking precedence over `tonemap`.
//   - Hook::DirectLighting: THE definition of osgx_DirectLighting().
//   - Hook::DeferredLighting: REPLACES the pass's entire fragment main(). A custom shader pulls
//     `#pragma osgx::gbuffer DEFERRED_LIGHTING_INPUTS, GET_GBUFFER` plus whatever other catalogs
//     it wants, and must be passed through resolveShaderLibs() before wrapping it in an
//     osg::Shader (see examples/osgx-gbuffer-comic.cpp).
//   Each is a substitution: GLSL permits one body per function (and one main()), so the pass always
//   attaches exactly one shader object per slot.
// - `shadowMap` (nullptr = unshadowed) swaps in DIRECT_LIGHTING_HOOK_SHADOWED and binds its depth
//   texture (at the "osgx::shadowMap" slot) and uniforms.
// - `aoTexture`, if set, is multiplied into the ambient term (osgx::SSAO::create()'s output, or any
//   other occlusion source).
struct PBRLightingPassOptions {
	osgx::Environment* environment = nullptr;
	bool tonemap = true;
	osgx::HookList hooks = {};
	const osgx::ShadowMap* shadowMap = nullptr;
	osg::Texture2D* aoTexture = nullptr;
	bool diagnostics = false;
};

struct PBRLightingPass {
	osg::ref_ptr<osg::Node> node;
	// The environment from PBRLightingPassOptions, attached to the pass; null if none.
	osg::ref_ptr<osgx::Environment> environment;
	// The main camera's view matrix and its inverse, refreshed by update().
	osg::ref_ptr<osg::Uniform> mainViewMatrix;
	osg::ref_ptr<osg::Uniform> mainViewMatrixInverse;

	bool valid() const;

	// A POST_RENDER fullscreen-quad pass reading `gbuffer`. `mainCamera` is the on-screen camera
	// whose view matrix rotates the G-buffer's view-space values into world space: the quad's own
	// camera is ABSOLUTE_RF with identity matrices, so osg_ViewMatrix is identity there.
	static PBRLightingPass create(
		const PBRGBuffer& gbuffer,
		osg::Camera* mainCamera,
		const PBRLightingPassOptions& options={}
	);

	// Refreshes mainViewMatrix/mainViewMatrixInverse from `mainCamera`. Call it from a
	// preDrawCallback on the FIRST PRE_RENDER camera in the scene graph (by render order): every
	// PRE_RENDER camera draws before mainCamera's own preDrawCallback fires, and a call after
	// frame() returns is a frame later still, so either hands the pass a stale matrix (artifacts
	// that worsen while the camera moves).
	void update(const osg::Camera* mainCamera);
};

}
