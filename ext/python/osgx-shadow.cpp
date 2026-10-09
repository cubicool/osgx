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
	m.attr("SHADOW_FACTOR_DECL") = osgx::SHADOW_FACTOR_DECL;
	m.attr("SHADOW_FACTOR_HOOK_NONE") = osgx::SHADOW_FACTOR_HOOK_NONE;
	m.attr("SHADOW_UNIFORMS_MULTI") = osgx::SHADOW_UNIFORMS_MULTI;
	m.attr("SHADOW_FACTOR_2D") = osgx::SHADOW_FACTOR_2D;
	m.attr("SHADOW_FACTOR_CUBE") = osgx::SHADOW_FACTOR_CUBE;
	m.attr("SHADOW_FACTOR_HOOK_MULTI") = osgx::SHADOW_FACTOR_HOOK_MULTI;
	m.attr("MAX_SHADOWED_2D") = osgx::MAX_SHADOWED_2D;
	m.attr("MAX_SHADOWED_CUBE") = osgx::MAX_SHADOWED_CUBE;

	// makeShadowedDirectLightingHookShader()/makeShadowedPointDirectLightingHookShader() are gone
	// (2026-10-02) - DIRECT_LIGHTING_HOOK_SHADOWED/_POINT no longer exist; "shadowed or not" is now
	// the separate Hook.ShadowFactor slot (osgx.ShadowSet owns its real override, see below), and
	// osgx_DirectLighting() itself is unconditionally osgx.pbr's DIRECT_LIGHTING_HOOK_DEFAULT -
	// nothing left to build a "shadowed direct-lighting hook" shader FOR.

	// Hook.ShadowFactor's DEFAULT as a standalone shader object - the Python-side convenience
	// mirroring osgx.makeDirectLightingHookShader() (Light.hpp), for a caller hand-assembling a
	// Program outside PBRScene/PBRLightingPass (which both already wire this in automatically when
	// no ShadowSet is given). An osgx.ShadowSet's own `shader` is the override counterpart - no
	// helper needed there, it is just an ordinary attribute read.
	m.def(
		"makeShadowFactorNoneHookShader",
		[]() {
			auto* shader = new osg::Shader(
				osg::Shader::FRAGMENT,
				osgx::resolveShaderLibs(std::string(osgx::SHADOW_FACTOR_HOOK_NONE))
			);

			shader->setName("shadowFactorHook");

			return osg::ref_ptr<osg::Shader>(shader);
		},
		"Builds Hook.ShadowFactor's default-definition FRAGMENT shader object (always unshadowed, "
		"no shadow-map uniforms or samplers) - add it to a Program alongside "
		"osgx.makeDirectLightingHookShader()'s output when hand-assembling a Program with no "
		"osgx.ShadowSet. An osgx.ShadowSet's own `.shader` is the override counterpart."
	);

	auto shadowMap = py::class_<osgx::ShadowMap>(
		m,
		"ShadowMap",
		"A directional (create()), spot (createSpot()), or point (createPoint()) light's shadow "
		"map. Directional/spot own a single PRE_RENDER depth-only camera plus the uniforms "
		"osgx.ShadowSet reads every frame (world-space, not eye-space - shadowMatrix is just "
		"lightProj * lightView, no per-frame main-camera dependency); point owns an omnidirectional "
		"distance CaptureCubeMap instead (see cubeCapture/casters/lightPosition). Add one or more "
		"ShadowMaps to an osgx.ShadowSet to actually shadow a scene - this struct alone has no "
		"shader wiring of its own anymore."
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
			[](
				const osg::Vec3& lightDirection,
				const osgx::ShadowMap::Coverage& coverage,
				const osgx::ShadowMap::Options& options,
				py::object hooks
			) {
				return osgx::ShadowMap::create(
					lightDirection,
					coverage,
					options,
					pyx::unpack_one_or_many<osgx::HookList::value_type>(hooks)
				);
			},
			"lightDirection"_a,
			"coverage"_a,
			"options"_a=osgx::ShadowMap::Options{},
			"hooks"_a=py::dict(),
			"Builds a directional shadow map (PRE_RENDER depth camera + shadow-matrix uniform) sized "
			"and placed to keep near:far depth precision sane regardless of scene scale. `camera` "
			"still needs adding to the scene graph by the caller. `hooks` may substitute "
			"osgx.Hook.Skinning (a VERTEX shader defining osgx_ApplySkin(), e.g. "
			"osgx.SKINNING_HOOK_LINEAR_BLEND wrapped in osgx.resolveShaderLibs()) - without it, a "
			"skinned caster's shadow is cast from its bind pose, static, while its visible render "
			"animates correctly."
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
			[](
				const osg::Vec3& position,
				const osg::Vec3& direction,
				float outerConeAngle,
				const osgx::ShadowMap::Coverage& coverage,
				const osgx::ShadowMap::Options& options,
				py::object hooks
			) {
				return osgx::ShadowMap::createSpot(
					position,
					direction,
					outerConeAngle,
					coverage,
					options,
					pyx::unpack_one_or_many<osgx::HookList::value_type>(hooks)
				);
			},
			"position"_a,
			"direction"_a,
			"outerConeAngle"_a,
			"coverage"_a,
			"options"_a=osgx::ShadowMap::Options{},
			"hooks"_a=py::dict(),
			"Builds a spot light's shadow map: a PERSPECTIVE depth camera at `position` looking "
			"along `direction`, covering `outerConeAngle` (radians, half-angle, as "
			"LightSet.setSpot()). Near/far bracket the scene bound as seen from the light. A spot "
			"map usually wants a smaller bias than a directional one (non-linear depth). `hooks` - "
			"see create()'s own docstring; identical contract."
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
			[](
				const osg::Vec3& position,
				const osgx::ShadowMap::Coverage& coverage,
				int cubeSize,
				const osgx::ShadowMap::Options& options,
				py::object hooks
			) {
				return osgx::ShadowMap::createPoint(
					position,
					coverage,
					cubeSize,
					options,
					pyx::unpack_one_or_many<osgx::HookList::value_type>(hooks)
				);
			},
			"position"_a,
			"coverage"_a,
			"cubeSize"_a=256,
			"options"_a=osgx::ShadowMap::Options{},
			"hooks"_a=py::dict(),
			"Builds a point light's shadow map: an omnidirectional distance CUBE MAP (six "
			"perspective views written by a distance-only Program) instead of a single 2D depth "
			"camera - a point light needs visibility in every direction. `cubeSize` is separate "
			"from ShadowMap.Options.size (unused here) since it's six real-time cameras, not one - "
			"start small (256, the default) and raise it once a demo's actually running. "
			"options.extent is unused (no ortho box); options.margin sizes the far plane the same "
			"way createSpot() does. Add point-shadow-casting geometry to the returned ShadowMap's "
			"`casters` group (not `camera`, which is None for a point map). `hooks` - see create()'s "
			"own docstring; identical contract."
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

	py::class_<osgx::ShadowSet>(
		m,
		"ShadowSet",
		"Aggregates however many ShadowMaps a scene has (any mix of directional/spot and point, up "
		"to MAX_SHADOWED_2D/MAX_SHADOWED_CUBE each - module-level constants) into the combined "
		"uniform arrays and Hook.ShadowFactor override `shader` that PBRScene.Options.shadowSet/"
		"PBRLightingPass.Options.shadowSet read. Replaces the old single `ShadowMap` field entirely "
		"- that design could shadow at most one light, ever (directional/spot and point were even "
		"mutually exclusive)."
	)
		.def(py::init<>(), "Constructs an empty, invalid ShadowSet; see ShadowSet.create().")
		.def_readonly(
			"shader", &osgx::ShadowSet::shader,
			"The Hook.ShadowFactor override shader object - set PBRScene.Options.shadowSet/"
			"PBRLightingPass.Options.shadowSet instead of using this directly unless building a "
			"fully custom Program."
		)
		.def("valid", &osgx::ShadowSet::valid, "True once create()d.")
		.def_static("create", &osgx::ShadowSet::create, "Builds an empty set ready for add().")
		.def(
			"add", &osgx::ShadowSet::add, "map"_a, py::keep_alive<1, 2>(),
			"Registers `map` in the next free slot of whichever array matches its kind (2D for a "
			"directional/spot map, cube for a point map, auto-detected), writing its CURRENT "
			"uniform values immediately. Raises if that kind's array is already full or `map` is "
			"itself invalid. `map` must outlive this ShadowSet (py::keep_alive enforces this on the "
			"Python side) - it is not copied, only its current values are, each time."
		)
		.def(
			"sync", &osgx::ShadowSet::sync,
			"Re-reads every already-add()ed slot's CURRENT uniform values from its own ShadowMap "
			"and re-uploads them - call after reposition()/repositionSpot()/repositionPoint() on "
			"any member map so a live-dragged light's shadow stays in sync."
		)
		.def(
			"apply", &osgx::ShadowSet::apply, "stateSet"_a,
			"Adds every uniform to `stateSet` and binds each active 2D/cube texture to its own "
			"texture unit. Call once after every add() this ShadowSet will ever receive; sync() "
			"alone is enough after that for live updates."
		)
	;
}

}
