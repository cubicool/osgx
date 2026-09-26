#include "osgx-python.hpp"
#include "osgx/Shadow.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Texture2D>
#include <osg/Uniform>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_shadow(py::module_& m) {
	m.attr("SHADOW_UNIFORMS") = osgx::SHADOW_UNIFORMS;
	m.attr("SHADOW_FACTOR") = osgx::SHADOW_FACTOR;
	m.attr("DIRECT_LIGHTING_HOOK_SHADOWED") = osgx::DIRECT_LIGHTING_HOOK_SHADOWED;

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

	py::class_<osgx::ShadowMapOptions>(
		m,
		"ShadowMapOptions",
		"Tuning knobs for ShadowMap.create()/reposition(): orthographic frustum size, depth "
		"precision, and shadow darkness."
	)
		.def(
			py::init<>(),
			"Constructs default options (size=1024, extent=0 i.e. auto, margin=1.3, bias=0.005, strength=0.7)."
		)
		.def_readwrite(
			"size", &osgx::ShadowMapOptions::size,
			"Shadow map texture resolution (width == height), in texels."
		)
		.def_readwrite(
			"extent", &osgx::ShadowMapOptions::extent,
			"Half-width, in world units, of the orthographic shadow frustum's box. 0 (default) "
			"derives it from sceneBoundRadius * margin."
		)
		.def_readwrite(
			"margin", &osgx::ShadowMapOptions::margin,
			"Multiplies sceneBoundRadius both when deriving a default `extent` and when sizing "
			"near/far planes, keeping near:far depth precision bounded regardless of scene scale."
		)
		.def_readwrite(
			"bias", &osgx::ShadowMapOptions::bias,
			"Depth-comparison bias added during the shadow test to avoid self-shadowing artifacts."
		)
		.def_readwrite(
			"strength", &osgx::ShadowMapOptions::strength,
			"How dark a shadowed fragment gets: 0 = shadows have no effect, 1 = fully black."
		)
	;

	py::class_<osgx::ShadowMap>(
		m,
		"ShadowMap",
		"A directional shadow map: owns the PRE_RENDER depth-only orthographic camera plus the "
		"uniforms DIRECT_LIGHTING_HOOK_SHADOWED reads every frame. World-space, not eye-space - "
		"shadowMatrix is just lightProj * lightView, with no per-frame main-camera dependency."
	)
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
		.def("valid", &osgx::ShadowMap::valid, "True if camera and depthTexture were successfully built.")
		.def_static(
			"create",
			&osgx::ShadowMap::create,
			"lightDirection"_a,
			"sceneBoundCenter"_a,
			"sceneBoundRadius"_a,
			"options"_a=osgx::ShadowMapOptions{},
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
			&osgx::ShadowMap::reposition,
			"lightDirection"_a,
			"sceneBoundCenter"_a,
			"sceneBoundRadius"_a,
			"options"_a=osgx::ShadowMapOptions{},
			"Repositions this EXISTING ShadowMap for a new light direction/scene bound, in place - "
			"no new camera/FBO/depth-texture allocation, just recomputed view/projection matrices. "
			"Cheap enough to call every frame (or on every GUI-slider tick) for an interactively-moving "
			"light; create() remains correct for a light fixed at scene-build time."
		)
		.def_static(
			"createSpot",
			&osgx::ShadowMap::createSpot,
			"position"_a,
			"direction"_a,
			"outerConeAngle"_a,
			"sceneBoundCenter"_a,
			"sceneBoundRadius"_a,
			"options"_a=osgx::ShadowMapOptions{},
			"Builds a spot light's shadow map: a PERSPECTIVE depth camera at `position` looking "
			"along `direction`, covering `outerConeAngle` (radians, half-angle, as "
			"LightSet.setSpot()). Near/far bracket the scene bound as seen from the light. A spot "
			"map usually wants a smaller bias than a directional one (non-linear depth)."
		)
		.def(
			"repositionSpot",
			&osgx::ShadowMap::repositionSpot,
			"position"_a,
			"direction"_a,
			"outerConeAngle"_a,
			"sceneBoundCenter"_a,
			"sceneBoundRadius"_a,
			"options"_a=osgx::ShadowMapOptions{},
			"reposition()'s counterpart for a createSpot() map."
		)
	;
}

}
