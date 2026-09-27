#include "osgx-python.hpp"
#include "osgx/CaptureCubeMap.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Group>
#include <osg/Program>
#include <osg/TextureCubeMap>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_capturecubemap(py::module_& m) {
	py::enum_<osgx::CaptureCubeMapFormat>(
		m,
		"CaptureCubeMapFormat",
		"What each of a CaptureCubeMapScene's six capture cameras writes to radianceTexture - "
		"Radiance (RGB16F, every IBL/environment-probe caller) or Distance (R32F, a single linear "
		"distance-from-light value per texel, ShadowMap.createPoint()'s own use)."
	)
		.value("Radiance", osgx::CaptureCubeMapFormat::Radiance)
		.value("Distance", osgx::CaptureCubeMapFormat::Distance)
	;

	py::class_<osgx::CaptureCubeMapOptions>(
		m,
		"CaptureCubeMapOptions",
		"Generic cubemap capture settings for CaptureCubeMapScene.create()/recapture(). Filtering, "
		"file formats, and lighting semantics are deliberately outside this layer - it only renders "
		"a scene through six ordinary perspective views."
	)
		.def(
			py::init<>(),
			"Constructs default options (cubeSize=256, nearPlane=0.1, farPlane=1000.0, "
			"format=Radiance, overrideProgram=None, continuous=False)."
		)
		.def_readwrite("cubeSize", &osgx::CaptureCubeMapOptions::cubeSize, "Cube face resolution (width == height), in texels.")
		.def_readwrite("nearPlane", &osgx::CaptureCubeMapOptions::nearPlane, "Near clip plane shared by all six 90 degree perspective views.")
		.def_readwrite("farPlane", &osgx::CaptureCubeMapOptions::farPlane, "Far clip plane shared by all six 90 degree perspective views.")
		.def_readwrite("clearColor", &osgx::CaptureCubeMapOptions::clearColor, "Clear color/value applied to each face before rendering.")
		.def_readwrite("format", &osgx::CaptureCubeMapOptions::format, "Radiance (RGB16F) or Distance (R32F) - see CaptureCubeMapFormat.")
		.def_readwrite(
			"overrideProgram", &osgx::CaptureCubeMapOptions::overrideProgram,
			"ON|OVERRIDE Program forced onto every capture camera's own StateSet, or None (the "
			"default) to let the captured scene render with its own real materials, as a "
			"radiance/IBL probe needs."
		)
		.def_readwrite(
			"continuous", &osgx::CaptureCubeMapOptions::continuous,
			"False (default): a one-shot bake - each camera renders once via RunOnceCallback, then "
			"goes idle until recapture() re-arms it. True: no RunOnceCallback at all - the six "
			"cameras render every frame like any ordinary camera, for a caster/light that keeps "
			"moving."
		)
	;

	py::class_<osgx::CaptureCubeMapScene>(
		m,
		"CaptureCubeMapScene",
		"A retained, frame-driven cubemap capture: six PRE_RENDER perspective cameras. Add root to "
		"a rendered scene graph and advance frames; ready() becomes true once all six faces have "
		"rendered at least once. The captured node may also belong to the application's visible "
		"scene."
	)
		.def(py::init<>(), "Constructs an empty, invalid scene; see CaptureCubeMapScene.create().")
		.def_readonly("root", &osgx::CaptureCubeMapScene::root, "The six capture cameras; add to a rendered scene graph.")
		.def_readonly(
			"radianceTexture", &osgx::CaptureCubeMapScene::radianceTexture,
			"The captured TextureCubeMap - Radiance or Distance content, per whichever "
			"CaptureCubeMapOptions.format create() was given."
		)
		.def("ready", &osgx::CaptureCubeMapScene::ready, "True once all six faces have rendered at least once.")
		.def_static(
			"create",
			&osgx::CaptureCubeMapScene::create,
			"capturedNode"_a,
			"position"_a,
			"options"_a=osgx::CaptureCubeMapOptions{},
			"Builds the six-camera capture rig around `capturedNode`, positioned at `position`. "
			"Renders nothing until root is added to a rendered scene graph."
		)
		.def(
			"recapture", &osgx::CaptureCubeMapScene::recapture, "position"_a,
			"Reuses the existing six cameras/output texture for a new capture position - the "
			"captured node itself is unchanged; mutate that scene normally before requesting "
			"another capture."
		)
	;
}

}
