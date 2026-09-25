#pragma once

#include "osgx/Environment.hpp"
#include "osgx/Shader.hpp"
#include "osgx/Shadow.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Node>
#include <osg/Uniform>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

namespace osgx {

// ================================================================================================
// Forward PBR for osgx::Material geometry: one Program (PBR_VERTEX_SHADER plus a fragment shader)
// shading each material by whichever light sources are present - an osgx::Environment (image-based
// light), the osgx::LightSet direct lights inherited from the scene graph, and an optional
// osgx::ShadowMap for the key light. The deferred counterpart is PBRGBuffer/PBRLightingPass
// (PBRDeferred.hpp), which takes the same light sources the same way.
//
// StateSet defines the fragment shader imports:
//   OSGX_PBR_ENVIRONMENT - an environment was given; without it the environment term is zero and
//                          the surface is lit by the direct lights and its emissive alone.
//   OSGX_PBR_DIAGNOSTICS - PBRSceneOptions::diagnostics is true.
// ================================================================================================

// - `environment`: attached to the node's StateSet. The caller owns it, may share it between
//   scenes, and adds its getBakeRoot() to the graph if non-null; its intensities and rotation stay
//   live-tunable.
// - `shadowMap`: swaps in DIRECT_LIGHTING_HOOK_SHADOWED and binds the depth texture (at the
//   "osgx::shadowMap" slot) and shadow uniforms. The caller builds the ShadowMap (its light
//   direction must match the osgx::LightSet light at ShadowMap::casterIndex) and adds its camera to
//   the scene graph.
// - `hooks` (HookList, Shader.hpp) substitutes the Hook::Skinning (osgx_ApplySkin(), e.g.
//   SKINNING_HOOK_LINEAR_BLEND) and Hook::Tonemap (osgx_Tonemap()) shader objects. Each REPLACES its
//   built-in; GLSL permits one body per function.
// - `diagnostics`: adds the debugMode/disableNormalMap/disableRoughnessMap/disableSpecularAA
//   uniforms (PBRScene's fields). debugMode: 0 combined, 1 environment diffuse, 2 environment
//   specular, 3 base color, 4 roughness, 5 metallic, 6 normal texture, 7 raw normal texture,
//   8 geometry normal, 9 shading normal, 10 tangent, 11 bitangent, 12-14 linear diffuse/specular/
//   combined (no tone curve or gamma).
struct PBRSceneOptions {
	osgx::Environment* environment = nullptr;
	const osgx::ShadowMap* shadowMap = nullptr;
	osgx::HookList hooks = {};
	bool diagnostics = false;
};

struct PBRScene {
	osg::ref_ptr<osg::Node> node;
	// The environment from PBRSceneOptions, attached to `node`; null if none.
	osg::ref_ptr<osgx::Environment> environment;
	// Set only when PBRSceneOptions::diagnostics is true.
	osg::ref_ptr<osg::Uniform> debugMode;
	osg::ref_ptr<osg::Uniform> disableNormalMap;
	osg::ref_ptr<osg::Uniform> disableRoughnessMap;
	osg::ref_ptr<osg::Uniform> disableSpecularAA;

	bool valid() const;

	// Attaches the forward PBR Program (OVERRIDE) and the given light sources to `node`'s StateSet.
	static PBRScene create(osg::Node* node, const PBRSceneOptions& options={});
};

}
