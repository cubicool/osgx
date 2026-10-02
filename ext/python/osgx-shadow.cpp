#include "osgx-python.hpp"
#include "osgx/CaptureCubeMap.hpp"
#include "osgx/Shadow.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Group>
#include <osg/Texture2D>
#include <osg/Uniform>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_shadow(py::module_& m) {
	m.attr("SHADOW_UNIFORMS") = osgx::SHADOW_UNIFORMS;
	m.attr("SHADOW_FACTOR") = osgx::SHADOW_FACTOR;
	m.attr("DIRECT_LIGHTING_HOOK_SHADOWED") = osgx::DIRECT_LIGHTING_HOOK_SHADOWED;
	m.attr("SHADOW_UNIFORMS_POINT") = osgx::SHADOW_UNIFORMS_POINT;
	m.attr("SHADOW_FACTOR_POINT") = osgx::SHADOW_FACTOR_POINT;
	m.attr("DIRECT_LIGHTING_HOOK_SHADOWED_POINT") = osgx::DIRECT_LIGHTING_HOOK_SHADOWED_POINT;

	// Python-side convenience mirroring osgx.pbr.makeDirectLightingHookShader() - assembles
	// DIRECT_LIGHTING_HOOK_SHADOWED as a standalone FRAGMENT osg::Shader, ready to add()/append()
	// onto an osg::Program in place of osgx.pbr.makeDirectLightingHookShader()'s unshadowed one.
	m.def(
		"makeShadowedDirectLightingHookShader",
		[]() {
			auto* shader = new osg::Shader(
				osg::Shader::FRAGMENT,
				osgx::resolveShaderLibs(std::string(osgx::DIRECT_LIGHTING_HOOK_SHADOWED))
			);

			// Same bare role name as osgx.pbr.makeDirectLightingHookShader() - this fills the
			// same logical slot, just with the shadowed implementation.
			shader->setName("directLightingHook");

			return osg::ref_ptr<osg::Shader>(shader);
		},
		"Builds the osgx_DirectLighting() CONTRACT's shadowed-definition FRAGMENT shader object - "
		"same contract as osgx.pbr.makeDirectLightingHookShader(), but the light at "
		"osgx_shadowCasterIndex is multiplied by osgx_ShadowFactor()."
	);

	// The point-light counterpart - identical shape, DIRECT_LIGHTING_HOOK_SHADOWED_POINT instead
	// (cube-map distance lookup via osgx_ShadowFactorPoint() rather than a 2D shadowMatrix).
	m.def(
		"makeShadowedPointDirectLightingHookShader",
		[]() {
			auto* shader = new osg::Shader(
				osg::Shader::FRAGMENT,
				osgx::resolveShaderLibs(std::string(osgx::DIRECT_LIGHTING_HOOK_SHADOWED_POINT))
			);

			shader->setName("directLightingHook");

			return osg::ref_ptr<osg::Shader>(shader);
		},
		"Builds the osgx_DirectLighting() CONTRACT's point-light-shadowed-definition FRAGMENT "
		"shader object - same contract as makeDirectLightingHookShader(), but the light at "
		"osgx_shadowCasterIndex is multiplied by osgx_ShadowFactorPoint() (a distance cube-map "
		"lookup, for a ShadowMap.createPoint() map)."
	);

	auto shadowMap = py::class_<osgx::ShadowMap>(
		m,
		"ShadowMap",
		"A directional (create()), spot (createSpot()), or point (createPoint()) light's shadow "
		"map. Directional/spot own a single PRE_RENDER depth-only camera plus the uniforms "
		"DIRECT_LIGHTING_HOOK_SHADOWED reads every frame (world-space, not eye-space - shadowMatrix "
		"is just lightProj * lightView, no per-frame main-camera dependency); point owns an "
		"omnidirectional distance CaptureCubeMap instead (see cubeCapture/casters/"
		"lightPosition), read by DIRECT_LIGHTING_HOOK_SHADOWED_POINT."
	);

	py::class_<osgx::ShadowMap::Coverage>(
		shadowMap,
		"Coverage",
		"The scene-bound pair every create()/createSpot()/createPoint()/reposition*() call needs - "
		"bundled here instead of separate positional arguments. `center`/`radius` is the CASTER "
		"bound (what must be rendered into the depth map); `receiverCenter`/`receiverRadius` is an "
		"optional, separate RECEIVER bound (what must be able to sample it correctly, e.g. a floor "
		"extending past the casting model) - leave receiverRadius at 0 (default) if receivers never "
		"extend past the caster bound itself."
	)
		.def(
			py::init<>(),
			"Constructs default coverage (center=(0,0,0), radius=1, no separate receiver bound)."
		)
		.def(
			py::init<const osg::Vec3&, float>(),
			"center"_a,
			"radius"_a,
			"Constructs coverage from an explicit caster center/radius, no separate receiver bound."
		)
		.def_readwrite("center", &osgx::ShadowMap::Coverage::center, "World-space caster-bound center.")
		.def_readwrite("radius", &osgx::ShadowMap::Coverage::radius, "World-space caster-bound radius.")
		.def_readwrite(
			"receiverCenter", &osgx::ShadowMap::Coverage::receiverCenter,
			"World-space receiver-bound center - only meaningful when receiverRadius > 0."
		)
		.def_readwrite(
			"receiverRadius", &osgx::ShadowMap::Coverage::receiverRadius,
			"World-space receiver-bound radius. 0 (default) means receivers never extend past the "
			"caster bound above."
		)
		.def(
			"bound",
			&osgx::ShadowMap::Coverage::bound,
			"The single osg.BoundingSphere every create()/reposition*() fitting call actually sizes "
			"its frustum/far-plane against - the caster bound, expanded to also enclose the receiver "
			"bound when receiverRadius > 0."
		)
	;

	py::class_<osgx::ShadowMap::Options>(
		shadowMap,
		"Options",
		"Tuning knobs for ShadowMap.create()/reposition(): orthographic frustum size, depth "
		"precision, and shadow darkness."
	)
		.def(
			py::init<>(),
			"Constructs default options (size=1024, extent=0 i.e. auto, margin=1.3, bias=0.005, strength=0.7)."
		)
		.def_readwrite(
			"size", &osgx::ShadowMap::Options::size,
			"Shadow map texture resolution (width == height), in texels."
		)
		.def_readwrite(
			"extent", &osgx::ShadowMap::Options::extent,
			"Half-width, in world units, of the orthographic shadow frustum's box. 0 (default) "
			"derives it from coverage.bound().radius() * margin - prefer Coverage.receiverCenter/"
			"receiverRadius over setting this directly."
		)
		.def_readwrite(
			"margin", &osgx::ShadowMap::Options::margin,
			"Multiplies coverage.bound().radius() both when deriving a default `extent` and when sizing "
			"near/far planes, keeping near:far depth precision bounded regardless of scene scale."
		)
		.def_readwrite(
			"bias", &osgx::ShadowMap::Options::bias,
			"Depth-comparison bias added during the shadow test to avoid self-shadowing artifacts."
		)
		.def_readwrite(
			"strength", &osgx::ShadowMap::Options::strength,
			"How dark a shadowed fragment gets: 0 = shadows have no effect, 1 = fully black."
		)
	;

	shadowMap
		.def(py::init<>(), "Constructs an empty ShadowMap with no camera/textures set; see ShadowMap.create().")
		.def_readwrite(
			"camera", &osgx::ShadowMap::camera,
			"The PRE_RENDER depth-only orthographic camera; add it to the scene graph."
		)
		.def_readwrite(
			"depthTexture", &osgx::ShadowMap::depthTexture,
			"The rendered depth attachment, sampled by osgx_ShadowFactor()."
		)
		.def_readwrite(
			"shadowMatrix", &osgx::ShadowMap::shadowMatrix,
			"world space -> light clip space. Set by create(); recompute via updateMatrix() if "
			"lightView/lightProj change directly."
		)
		.def_readwrite(
			"bias", &osgx::ShadowMap::bias,
			"Depth-comparison bias uniform read by osgx_ShadowFactor()."
		)
		.def_readwrite(
			"normalOffset", &osgx::ShadowMap::normalOffset,
			"World-space distance applied along the receiver's own normal before the light-space "
			"transform, derived from this map's own texel footprint at create()/reposition() time. "
			"Prefer tuning this over `bias` for ordinary acne/peter-panning."
		)
		.def_readwrite(
			"strength", &osgx::ShadowMap::strength,
			"Shadow darkness uniform: 0 = shadows have no effect, 1 = fully black."
		)
		.def_readwrite(
			"casterIndex", &osgx::ShadowMap::casterIndex,
			"Which osgx_lights[] index this shadow map is cast by - only that light's "
			"contribution is multiplied by osgx_ShadowFactor(); every other light is unaffected."
		)
		.def_readwrite(
			"lightView", &osgx::ShadowMap::lightView,
			"The shadow camera's view matrix, as last computed by create()/reposition()."
		)
		.def_readwrite(
			"lightProj", &osgx::ShadowMap::lightProj,
			"The shadow camera's projection matrix, as last computed by create()/reposition()."
		)
		.def_readwrite(
			"cubeCapture", &osgx::ShadowMap::cubeCapture,
			"Point-light-only (see createPoint()) - the six-camera distance-cube capture rig. "
			"Invalid/unused for a directional or spot map."
		)
		.def_readwrite(
			"casters", &osgx::ShadowMap::casters,
			"Point-light-only (see createPoint()) - createPoint()'s counterpart to adding children "
			"directly to `camera` for the other two kinds: add point-shadow-casting geometry here "
			"instead. None for a directional or spot map."
		)
		.def_readwrite(
			"lightPosition", &osgx::ShadowMap::lightPosition,
			"Point-light-only (see createPoint()) - world-space light position uniform "
			"osgx_ShadowFactorPoint() reads. None for a directional or spot map."
		)
		.def("valid", &osgx::ShadowMap::valid, "True if camera and depthTexture were successfully built.")
		.def_static(
			"create",
			static_cast<osgx::ShadowMap (*) (
				const osg::Vec3&, const osgx::ShadowMap::Coverage&, const osgx::ShadowMap::Options&
			)>(&osgx::ShadowMap::create),
			"lightDirection"_a,
			"coverage"_a,
			"options"_a=osgx::ShadowMap::Options{},
			"Builds a directional shadow map (PRE_RENDER depth camera + shadow-matrix uniform) sized "
			"and placed to keep near:far depth precision sane regardless of scene scale. `camera` "
			"still needs adding to the scene graph by the caller."
		)
		.def(
			"updateMatrix",
			&osgx::ShadowMap::updateMatrix,
			"Recomputes shadowMatrix from lightView/lightProj - only needed after mutating "
			"either directly; a no-op to call redundantly otherwise. Does NOT reposition the camera "
			"itself - see reposition() for that."
		)
		.def(
			"reposition",
			static_cast<void (osgx::ShadowMap::*) (
				const osg::Vec3&, const osgx::ShadowMap::Coverage&, const osgx::ShadowMap::Options&
			)>(&osgx::ShadowMap::reposition),
			"lightDirection"_a,
			"coverage"_a,
			"options"_a=osgx::ShadowMap::Options{},
			"Repositions this EXISTING ShadowMap for a new light direction/scene bound, in place - "
			"no new camera/FBO/depth-texture allocation, just recomputed view/projection matrices. "
			"Cheap enough to call every frame (or on every GUI-slider tick) for an interactively-moving "
			"light; create() remains correct for a light fixed at scene-build time."
		)
		.def_static(
			"createSpot",
			static_cast<osgx::ShadowMap (*) (
				const osg::Vec3&, const osg::Vec3&, float, const osgx::ShadowMap::Coverage&,
				const osgx::ShadowMap::Options&
			)>(&osgx::ShadowMap::createSpot),
			"position"_a,
			"direction"_a,
			"outerConeAngle"_a,
			"coverage"_a,
			"options"_a=osgx::ShadowMap::Options{},
			"Builds a spot light's shadow map: a PERSPECTIVE depth camera at `position` looking "
			"along `direction`, covering `outerConeAngle` (radians, half-angle, as "
			"LightSet.setSpot()). Near/far bracket the scene bound as seen from the light. A spot "
			"map usually wants a smaller bias than a directional one (non-linear depth)."
		)
		.def(
			"repositionSpot",
			static_cast<void (osgx::ShadowMap::*) (
				const osg::Vec3&, const osg::Vec3&, float, const osgx::ShadowMap::Coverage&,
				const osgx::ShadowMap::Options&
			)>(&osgx::ShadowMap::repositionSpot),
			"position"_a,
			"direction"_a,
			"outerConeAngle"_a,
			"coverage"_a,
			"options"_a=osgx::ShadowMap::Options{},
			"reposition()'s counterpart for a createSpot() map."
		)
		.def_static(
			"createPoint",
			static_cast<osgx::ShadowMap (*) (
				const osg::Vec3&, const osgx::ShadowMap::Coverage&, int, const osgx::ShadowMap::Options&
			)>(&osgx::ShadowMap::createPoint),
			"position"_a,
			"coverage"_a,
			"cubeSize"_a=256,
			"options"_a=osgx::ShadowMap::Options{},
			"Builds a point light's shadow map: an omnidirectional distance CUBE MAP (six "
			"perspective views written by a distance-only Program) instead of a single 2D depth "
			"camera - a point light needs visibility in every direction. `cubeSize` is separate "
			"from ShadowMap.Options.size (unused here) since it's six real-time cameras, not one - "
			"start small (256, the default) and raise it once a demo's actually running. "
			"options.extent is unused (no ortho box); options.margin sizes the far plane the same "
			"way createSpot() does. Add point-shadow-casting geometry to the returned ShadowMap's "
			"`casters` group (not `camera`, which is None for a point map)."
		)
		.def(
			"repositionPoint",
			static_cast<void (osgx::ShadowMap::*) (
				const osg::Vec3&, const osgx::ShadowMap::Coverage&, const osgx::ShadowMap::Options&
			)>(&osgx::ShadowMap::repositionPoint),
			"position"_a,
			"coverage"_a,
			"options"_a=osgx::ShadowMap::Options{},
			"reposition()/repositionSpot()'s counterpart for a createPoint() map - re-aims the six "
			"capture cameras at a new position and refreshes their shared far plane/clear value for "
			"the new distance to the scene."
		)
	;
}

}
