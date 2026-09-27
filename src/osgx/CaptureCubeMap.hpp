#pragma once

#include "IBL.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Group>
#include <osg/Node>
#include <osg/Program>
#include <osg/TextureCubeMap>
#include <osg/Vec3d>
#include <osg/Vec4>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <array>

namespace osgx {

// What each of the six capture cameras writes to `radianceTexture` (the field keeps its name
// regardless - see the struct's own comment). Radiance is every existing caller (IBL/environment
// probes); Distance is osgx::ShadowMap::createPoint()'s own use, a single-channel linear
// distance-from-light cube, since a point light's shadow test has no 2D depth map to compare
// against and needs real distance in every direction instead.
enum class CaptureCubeMapFormat {
	Radiance,
	Distance
};

// Generic cubemap capture settings. Filtering, file formats, and lighting semantics are
// deliberately outside this layer: it only renders a scene through six ordinary perspective views.
struct CaptureCubeMapOptions {
	int cubeSize = 256;
	double nearPlane = 0.1;
	double farPlane = 1000.0;
	osg::Vec4 clearColor = osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f);

	// See CaptureCubeMapFormat above.
	CaptureCubeMapFormat format = CaptureCubeMapFormat::Radiance;

	// ON|OVERRIDE on every capture camera's own StateSet when set - lets a caller replace whatever
	// Program the captured scene's own StateSet would otherwise render with (e.g. ShadowMap's
	// distance-only Program). Null (the default) keeps every existing caller's behavior: the
	// captured scene renders with its own real materials, as a radiance/IBL probe needs.
	osg::ref_ptr<osg::Program> overrideProgram;

	// false (the default): a one-shot bake - each camera renders once via RunOnceCallback, then
	// goes idle until recapture() explicitly re-arms it (today's IBL/environment-probe behavior).
	// true: no RunOnceCallback at all - the six cameras render every frame like any ordinary
	// PRE_RENDER camera, for a caster/light that keeps moving (ShadowMap::createPoint()'s use).
	// recapture() still matters in this mode, just for re-aiming the view (a moved light), not for
	// forcing a render.
	bool continuous = false;
};

// A retained, frame-driven cubemap capture. Add root to a rendered scene graph and advance frames;
// ready() becomes true after all six faces have rendered once. The captured node may also belong to
// the application's visible scene. Capture cameras skip child update traversal, so shared update
// callbacks run through the application's normal scene path rather than six times per capture.
struct CaptureCubeMapScene {
	osg::ref_ptr<osg::Group> root;
	// Despite the name, this holds whatever CaptureCubeMapOptions::format asked for - a distance
	// cube (CaptureCubeMapFormat::Distance) uses this same field, not a separate one.
	osg::ref_ptr<osg::TextureCubeMap> radianceTexture;
	osg::ref_ptr<BakeCompletion> completion;
	std::array<osg::ref_ptr<osg::Camera>, 6> cameras;

	bool ready() const;

	static CaptureCubeMapScene create(
		osg::Node* capturedNode,
		const osg::Vec3d& position,
		const CaptureCubeMapOptions& options={}
	);

	// Reuses the existing six cameras and output texture from a new capture position. The captured
	// node itself remains unchanged; mutate that scene normally before requesting another capture.
	bool recapture(const osg::Vec3d& position);
};

}
