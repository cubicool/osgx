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
// osgx::ShadowSet shadowing any mix of them. The deferred counterpart is PBRGBuffer/PBRLightingPass
// (PBRDeferred.hpp), which takes the same light sources the same way.
//
// StateSet defines the fragment shader imports:
//   OSGX_PBR_ENVIRONMENT - an environment was given; without it the environment term is zero and
//                          the surface is lit by the direct lights and its emissive alone.
//   OSGX_PBR_DIAGNOSTICS - PBRScene::Options::diagnostics is true.
// ================================================================================================

// - `environment`: attached to the node's StateSet. The caller owns it, may share it between
//   scenes, and adds its getBakeRoot() to the graph if non-null; its intensities and rotation stay
//   live-tunable.
// - `shadowSet`: swaps in the real Hook::ShadowFactor override (osgx::ShadowSet's own `shader`)
//   and binds its combined depth/cube textures and uniforms - see osgx::ShadowSet's own comment
//   (Shadow.hpp) for the full design. The caller builds however many osgx::ShadowMaps the scene
//   needs, osgx::ShadowSet::add()s each (their light directions/positions must match the
//   osgx::LightSet light at that map's own ShadowMap::casterIndex), and adds each map's own camera
//   (or cubeCapture.root, for a point map) to the scene graph.
// - `hooks` (HookList, Shader.hpp) substitutes the Hook::Skinning (osgx_ApplySkin(), e.g.
//   SKINNING_HOOK_LINEAR_BLEND) and Hook::Tonemap (osgx_Tonemap()) shader objects. Each REPLACES its
//   built-in; GLSL permits one body per function.
// - `diagnostics`: adds the debugMode/disableNormalMap/disableRoughnessMap/disableSpecularAA
//   uniforms (PBRScene's fields). debugMode: 0 combined, 1 environment diffuse, 2 environment
//   specular, 3 base color, 4 roughness, 5 metallic, 6 normal texture, 7 raw normal texture,
//   8 geometry normal, 9 shading normal, 10 tangent, 11 bitangent, 12-14 linear diffuse/specular/
//   combined (no tone curve or gamma).
struct PBRScene {
	struct Options {
		osgx::Environment* environment = nullptr;
		const osgx::ShadowSet* shadowSet = nullptr;
		osgx::HookList hooks = {};
		bool diagnostics = false;
	};

	osg::ref_ptr<osg::Node> node;
	// The environment from Options, attached to `node`; null if none.
	osg::ref_ptr<osgx::Environment> environment;
	// Set only when Options::diagnostics is true.
	osg::ref_ptr<osg::Uniform> debugMode;
	osg::ref_ptr<osg::Uniform> disableNormalMap;
	osg::ref_ptr<osg::Uniform> disableRoughnessMap;
	osg::ref_ptr<osg::Uniform> disableSpecularAA;

	bool valid() const;

	// Attaches the forward PBR Program (OVERRIDE) and the given light sources to `node`'s StateSet.
	static PBRScene create(osg::Node* node, const Options& options);
	static PBRScene create(osg::Node* node);
};

}
