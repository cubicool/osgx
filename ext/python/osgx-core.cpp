#include "osgx-python.hpp"
#include "osgx/CameraIntents.hpp"
#include "osgx/Core.hpp"
#include "osgx/Grid.hpp"
#include "osgx/Manipulators.hpp"
#include "osgx/Shader.hpp"

namespace osgx_python {

// Only the TrackballManipulator instantiation is bound for now - the only Base actually verified
// working (examples/osgx-manipulator.cpp's "intents" mode).
using TrackballCameraManipulator = osgx::CameraManipulator<osgGA::TrackballManipulator>;

}

// osgx::CameraManipulator<Base>::_callbacks introspection - pyx::SequenceProxy over the read
// accessors added alongside this (osgx/Manipulators.hpp's getNumUpdateCameraCallbacks()/
// getUpdateCameraCallback()), modeled directly on osg::Program's SequenceTraits (OpenSceneGraph.py's
// pyosg/osg/Program.hpp). Deliberately get/del only, no set()/append(): adding still goes through
// addUpdateCameraCallback(cb, runOnce), which needs the extra bool a homogeneous-element sequence
// can't carry.
template<>
struct pyx::SequenceTraits<osgx_python::TrackballCameraManipulator> {
	using element_type = osg::Callback;
	using value_type = element_type*;

	static value_type from_python(py::handle h) { return h.cast<value_type>(); }

	static size_t size(const osgx_python::TrackballCameraManipulator* m) {
		return m->getNumUpdateCameraCallbacks();
	}

	static element_type* get(osgx_python::TrackballCameraManipulator* m, size_t i) {
		return m->getUpdateCameraCallback(static_cast<unsigned int>(i));
	}

	static void del(osgx_python::TrackballCameraManipulator* m, size_t i) {
		m->removeUpdateCameraCallback(m->getUpdateCameraCallback(static_cast<unsigned int>(i)));
	}
};

namespace osgx_python {

namespace detail {
	using CallbacksProxy = pyx::SequenceProxy<TrackballCameraManipulator>;
	using CallbacksStorage = pyx::ProxyStorageOSG<TrackballCameraManipulator, CallbacksProxy>;
}

void bind_core(py::module_& m) {
	m.def(
		"findDataFile",
		[](
			const std::string& filename,
			const std::vector<std::string>& candidates,
			const std::string& suffix
		) {
			const std::span<const std::string> patterns(candidates);
			const auto result = suffix.empty() ?
				osgx::findDataFile(filename, patterns) :
				osgx::findDataFile(filename, patterns, suffix)
			;

			return result.empty() ? std::string{} : result.string();
		},
		"filename"_a,
		"candidates"_a=std::vector<std::string>{},
		"suffix"_a="",
		"Find filename through OSG_FILE_PATH, optionally trying each {} substitution candidate. "
		"Returns an empty string when no file is found."
	);

	auto gridSettings = py::class_<
		osgx::GridSettings,
		osg::StateAttribute,
		osg::ref_ptr<osgx::GridSettings>
	>(
		m,
		"GridSettings",
		"Live-tunable Grid rendering parameters (line width/intervals/colors/modes), stored in a "
		"std430 SSBO. Normally accessed through a Grid's own property passthroughs - construct "
		"directly only to share one GridSettings across multiple Grid instances."
	);

	gridSettings
		.def(py::init<>(), "Constructs default grid settings.")
		.def_property(
			"canvasSize", &osgx::GridSettings::getCanvasSize, &osgx::GridSettings::setCanvasSize,
			"Size of the grid's canvas in local UV space."
		)
		.def_property(
			"gridInterval", &osgx::GridSettings::getGridInterval, &osgx::GridSettings::setGridInterval,
			"Spacing between minor grid lines, in canvas units."
		)
		.def_property(
			"gridIntervalStrong",
			&osgx::GridSettings::getGridIntervalStrong,
			&osgx::GridSettings::setGridIntervalStrong,
			"Spacing between major (strong) grid lines, in canvas units."
		)
		.def_property(
			"lineWidthPx", &osgx::GridSettings::getLineWidthPx, &osgx::GridSettings::setLineWidthPx,
			"Line width in screen pixels, used when lineMode is LINE_SCREEN_PIXELS."
		)
		.def_property(
			"lineWidth", &osgx::GridSettings::getLineWidth, &osgx::GridSettings::setLineWidth,
			"Line width in grid/world units, used when lineMode is LINE_GRID_UNITS."
		)
		.def_property(
			"edgeMode", &osgx::GridSettings::getEdgeMode, &osgx::GridSettings::setEdgeMode,
			"How lines exactly on the canvas boundary are handled - see Grid.EdgeMode."
		)
		.def_property(
			"lineMode", &osgx::GridSettings::getLineMode, &osgx::GridSettings::setLineMode,
			"Whether line width is measured in screen pixels or grid/world units - see Grid.LineMode."
		)
		.def_property(
			"colorBg", &osgx::GridSettings::getColorBg, &osgx::GridSettings::setColorBg,
			"Background fill color."
		)
		.def_property(
			"colorLine", &osgx::GridSettings::getColorLine, &osgx::GridSettings::setColorLine,
			"Minor grid line color."
		)
		.def_property(
			"colorLineStrong",
			&osgx::GridSettings::getColorLineStrong,
			&osgx::GridSettings::setColorLineStrong,
			"Major (strong) grid line color."
		)
	;

	auto grid = py::class_<
		osgx::Grid,
		osg::Geometry,
		osg::ref_ptr<osgx::Grid>
	>(
		m,
		"Grid",
		"Procedurally-generated, antialiased, view-aware grid lines on a single quad. Supports "
		"crisp constant screen-pixel lines for flat overlays and grid/world-unit line widths for "
		"perspective ground planes."
	);

	py::enum_<osgx::Grid::EdgeMode>(
		grid,
		"EdgeMode",
		"Boundary-line handling for lines that fall exactly on the canvas edge."
	)
		.value("EDGE_ASIS", osgx::Grid::EDGE_ASIS)
		.value("EDGE_HIDE", osgx::Grid::EDGE_HIDE)
		.value("EDGE_NUDGE", osgx::Grid::EDGE_NUDGE)
		.export_values()
	;

	py::enum_<osgx::Grid::LineMode>(
		grid,
		"LineMode",
		"Whether lineWidth/lineWidthPx is measured in grid/world units or constant screen pixels."
	)
		.value("LINE_SCREEN_PIXELS", osgx::Grid::LINE_SCREEN_PIXELS)
		.value("LINE_GRID_UNITS", osgx::Grid::LINE_GRID_UNITS)
		.export_values()
	;

	grid
		.def(
			py::init<>(),
			"Constructs a fullscreen NDC quad (XY plane, z=0, -1..1) - pairs with orthoCamera()/"
			"createOrthoCamera()."
		)
		.def(py::init<const osg::Vec3&, const osg::Vec3&, const osg::Vec3&>(),
			"corner"_a, "width_vec"_a, "height_vec"_a,
			"Constructs a quad at `corner` spanning `width_vec`/`height_vec` - NDC for a "
			"fullscreen overlay, or world-space for a real 3D ground plane."
		)
		.def_property(
			"canvasSize", &osgx::Grid::getCanvasSize, &osgx::Grid::setCanvasSize,
			"Size of the grid's canvas in local UV space."
		)
		.def_property(
			"gridInterval", &osgx::Grid::getGridInterval, &osgx::Grid::setGridInterval,
			"Spacing between minor grid lines, in canvas units."
		)
		.def_property(
			"gridIntervalStrong",
			&osgx::Grid::getGridIntervalStrong,
			&osgx::Grid::setGridIntervalStrong,
			"Spacing between major (strong) grid lines, in canvas units."
		)
		.def_property(
			"lineWidthPx", &osgx::Grid::getLineWidthPx, &osgx::Grid::setLineWidthPx,
			"Line width in screen pixels, used when lineMode is LINE_SCREEN_PIXELS."
		)
		.def_property(
			"lineWidth", &osgx::Grid::getLineWidth, &osgx::Grid::setLineWidth,
			"Line width in grid/world units, used when lineMode is LINE_GRID_UNITS."
		)
		.def_property(
			"edgeMode", &osgx::Grid::getEdgeMode, &osgx::Grid::setEdgeMode,
			"How lines exactly on the canvas boundary are handled."
		)
		.def_property(
			"lineMode", &osgx::Grid::getLineMode, &osgx::Grid::setLineMode,
			"Whether line width is measured in screen pixels or grid/world units."
		)
		.def_property("colorBg", &osgx::Grid::getColorBg, &osgx::Grid::setColorBg, "Background fill color.")
		.def_property("colorLine", &osgx::Grid::getColorLine, &osgx::Grid::setColorLine, "Minor grid line color.")
		.def_property(
			"colorLineStrong",
			&osgx::Grid::getColorLineStrong,
			&osgx::Grid::setColorLineStrong,
			"Major (strong) grid line color."
		)
		.def_property(
			"settings",
			static_cast<osgx::GridSettings* (osgx::Grid::*)()>(&osgx::Grid::getSettings),
			&osgx::Grid::setSettings,
			"This Grid's own GridSettings; assign a shared GridSettings to have multiple Grids "
			"track the same live parameters."
		)
		.def(
			"orthoCamera", &osgx::Grid::orthoCamera,
			"Wraps this Grid in a Geode, and that Geode in an ABSOLUTE_RF, PRE_RENDER, "
			"ortho2D(-1,1,-1,1) camera - the always-drawn-first fullscreen NDC setup. The caller "
			"still needs to set the main camera's clear mask to GL_DEPTH_BUFFER_BIT-only, or its "
			"default color clear will stomp this camera's paint."
		)
		.def_static(
			"createOrthoCamera", py::overload_cast<>(&osgx::Grid::createOrthoCamera),
			"Constructs a default fullscreen Grid and immediately wraps it via orthoCamera()."
		)
		.def_static(
			"createOrthoCamera",
			py::overload_cast<const osg::Vec3&, const osg::Vec3&, const osg::Vec3&>(
				&osgx::Grid::createOrthoCamera
			),
			"corner"_a, "width_vec"_a, "height_vec"_a,
			"Constructs a Grid at `corner`/`width_vec`/`height_vec` and immediately wraps it via orthoCamera()."
		)
		.def_static(
			"createSphere",
			&osgx::Grid::createSphere,
			"radius"_a=1.0f, "slices"_a=48, "stacks"_a=24,
			"Builds a UV sphere using this Grid's own shader and uniforms; duplicated seam/pole "
			"vertices keep grid coordinates continuous except at the intentional longitude wrap."
		)
		.def_static(
			"registerShaderLibs", &osgx::registerGridShaderLibs,
			"Registers Grid's GLSL snippets under the #pragma osgx::grid shader-library key."
		)
	;

	// osgx::Ortho2DManipulator / OrbitAxisManipulator / MultiCameraManipulator (osgx/Manipulators.hpp).
	// All three derive from osgGA::CameraManipulator, already registered by pyosgGA.cpp as
	// "CameraManipulator" - its node/matrix/inverseMatrix/homePosition properties and
	// home()/init()/handle() methods come along automatically via virtual dispatch, so only each
	// subclass's OWN new surface needs binding here.
	py::class_<
		osgx::Ortho2DManipulator,
		osgGA::CameraManipulator,
		osg::ref_ptr<osgx::Ortho2DManipulator>
	>(
		m,
		"Ortho2DManipulator",
		"An osgGA.CameraManipulator for orthographic 2D scenes, owning both view and projection "
		"matrices. Pan (drag), zoom (scroll), Shift+scroll pixel-nudge zoom, and Ctrl+drag 3D "
		"orbit are all supported; Space/Home resets to fit the node's bound."
	)
		.def(py::init<>(), "Constructs a manipulator with no node set; call setNode()/setCameraManipulator() before use.")
		.def_property(
			"pixelNudge",
			&osgx::Ortho2DManipulator::getPixelNudge,
			&osgx::Ortho2DManipulator::setPixelNudge,
			"Shift+scroll zoom step: how many screen pixels the visible boundary moves per click (default 8)."
		)
		.def_property(
			"wheelZoomFactor",
			&osgx::Ortho2DManipulator::getWheelZoomFactor,
			&osgx::Ortho2DManipulator::setWheelZoomFactor,
			"Geometric zoom factor applied to halfExtentY per plain-scroll wheel click (default 1.15)."
		)
		.def_property(
			"rotateSensitivity",
			&osgx::Ortho2DManipulator::getRotateSensitivity,
			&osgx::Ortho2DManipulator::setRotateSensitivity,
			"Sensitivity of Ctrl+drag 3D tilt (yaw/pitch) to mouse motion."
		)
		.def_property(
			"invertY",
			&osgx::Ortho2DManipulator::getInvertY,
			&osgx::Ortho2DManipulator::setInvertY,
			"Inverts the Y axis for Ctrl-drag 3D pitch only; plain pan is unaffected."
		)
		.def_property(
			"invertX",
			&osgx::Ortho2DManipulator::getInvertX,
			&osgx::Ortho2DManipulator::setInvertX,
			"Inverts the X axis for Ctrl-drag 3D yaw only; plain pan is unaffected."
		)
		.def_property(
			"planeNormal",
			&osgx::Ortho2DManipulator::getPlaneNormal,
			&osgx::Ortho2DManipulator::setPlaneNormal,
			"Normal of the unrotated 2D plane (default: +Z)."
		)
		.def_property(
			"screenUp",
			&osgx::Ortho2DManipulator::getScreenUp,
			&osgx::Ortho2DManipulator::setScreenUp,
			"Up direction of the unrotated 2D plane (default: +Y)."
		)
		.def_property(
			"center",
			&osgx::Ortho2DManipulator::getCenter,
			&osgx::Ortho2DManipulator::setCenter,
			"World-space focal point the view is centered on."
		)
		.def_property(
			"halfExtentY",
			&osgx::Ortho2DManipulator::getHalfExtentY,
			&osgx::Ortho2DManipulator::setHalfExtentY,
			"Half the visible world height; the actual zoom-level state, clamped by zoomLimits."
		)
		.def_property(
			"zoomLimits",
			&osgx::Ortho2DManipulator::getZoomLimits,
			[](osgx::Ortho2DManipulator& self, py::object obj) {
				auto vals = pyx::try_unpack_sequence<double, double>(obj);

				if(!vals) throw py::type_error(
					"Expected a (minHalfExtent, maxHalfExtent) sequence of length 2"
				);

				auto& [minH, maxH] = *vals;

				self.setZoomLimits(minH, maxH);
			},
			"(minHalfExtent, maxHalfExtent) clamp for halfExtentY."
		)
	;

	py::class_<
		osgx::OrbitAxisManipulator,
		osgGA::CameraManipulator,
		osg::ref_ptr<osgx::OrbitAxisManipulator>
	>(
		m,
		"OrbitAxisManipulator",
		"A \"turntable\" osgGA.CameraManipulator: orbits a fixed vertical guide line through the "
		"model's bounds, always looking level, dollying toward/away from that line on zoom. "
		"State is cylindrical: yaw, height (clamped to heightLimits), distance (clamped by coverageLimits)."
	)
		.def(py::init<>(), "Constructs a manipulator with no node set; call setNode()/setCameraManipulator() before use.")
		.def_property(
			"yawSensitivity",
			&osgx::OrbitAxisManipulator::getYawSensitivity,
			&osgx::OrbitAxisManipulator::setYawSensitivity,
			"Sensitivity of yaw rotation to raw pointer motion."
		)
		.def_property(
			"heightSensitivity",
			&osgx::OrbitAxisManipulator::getHeightSensitivity,
			&osgx::OrbitAxisManipulator::setHeightSensitivity,
			"Sensitivity of height changes to raw pointer motion."
		)
		.def_property(
			"wheelZoomFactor",
			&osgx::OrbitAxisManipulator::getWheelZoomFactor,
			&osgx::OrbitAxisManipulator::setWheelZoomFactor,
			"Dolly-distance factor applied per scroll wheel click."
		)
		.def_property(
			"invertY",
			&osgx::OrbitAxisManipulator::getInvertY,
			&osgx::OrbitAxisManipulator::setInvertY,
			"Inverts the pointer Y axis used for axial motion, in both the raw MOVE/DRAG path and orbitByDelta()."
		)
		.def_property(
			"upAxis",
			&osgx::OrbitAxisManipulator::getUpAxis,
			&osgx::OrbitAxisManipulator::setUpAxis,
			"Turntable up axis (default: +Z)."
		)
		.def_property(
			"homeDirection",
			&osgx::OrbitAxisManipulator::getHomeDirection,
			&osgx::OrbitAxisManipulator::setHomeDirection,
			"Horizontal camera direction at yaw == 0 (default: -Y)."
		)
		.def_property(
			"coverageLimits",
			&osgx::OrbitAxisManipulator::getCoverageLimits,
			[](osgx::OrbitAxisManipulator& self, py::object obj) {
				auto vals = pyx::try_unpack_sequence<double, double>(obj);

				if(!vals) throw py::type_error(
					"Expected a (minCoverage, maxCoverage) sequence of length 2"
				);

				auto& [minC, maxC] = *vals;

				self.setCoverageLimits(minC, maxC);
			},
			"(minCoverage, maxCoverage) viewport-coverage fractions at the zoom extremes."
		)
		.def_property(
			"heightLimits",
			&osgx::OrbitAxisManipulator::getHeightLimits,
			[](osgx::OrbitAxisManipulator& self, py::object obj) {
				auto vals = pyx::try_unpack_sequence<double, double>(obj);

				if(!vals) throw py::type_error(
					"Expected a (minHeight, maxHeight) sequence of length 2"
				);

				auto& [minHeight, maxHeight] = *vals;
				self.setHeightLimits(minHeight, maxHeight);
			},
			"(minHeight, maxHeight) world-space distances along upAxis that constrain camera height."
		)
		.def(
			"clearHeightLimits", &osgx::OrbitAxisManipulator::clearHeightLimits,
			"Removes any height clamp set via heightLimits, letting height move freely."
		)
		.def_property_readonly(
			"hasHeightLimits", &osgx::OrbitAxisManipulator::hasHeightLimits,
			"True if a height clamp is currently set."
		)
		.def_property(
			"liveOrbitEnabled",
			&osgx::OrbitAxisManipulator::isLiveOrbitEnabled,
			&osgx::OrbitAxisManipulator::setLiveOrbitEnabled,
			"Disable to drive orbit/height exclusively via orbitByDelta() (e.g. from "
			"osgx.platform.PointerCapture) instead of raw MOVE/DRAG cursor tracking."
		)
		.def_property_readonly("yaw", &osgx::OrbitAxisManipulator::getYaw, "Current yaw angle, in radians.")
		.def_property_readonly("height", &osgx::OrbitAxisManipulator::getHeight, "Current height along upAxis.")
		.def_property_readonly("distance", &osgx::OrbitAxisManipulator::getDistance, "Current dolly distance from the guide line.")
		.def(
			"orbitByDelta",
			&osgx::OrbitAxisManipulator::orbitByDelta,
			"dx"_a,
			"dy"_a,
			"Applies a pre-computed (dx, dy) directly, in the same normalized units as "
			"GUIEventAdapter.xNormalized/yNormalized."
		)
	;

	py::class_<
		osgx::MultiCameraManipulator,
		osgGA::CameraManipulator,
		osg::ref_ptr<osgx::MultiCameraManipulator>
	>(
		m,
		"MultiCameraManipulator",
		"A composite manipulator routing input to one active target (name, manipulator, optional "
		"dedicated camera/scene, optional setActive callback), with a key toggling between them."
	)
		.def(py::init<>(), "Constructs a manipulator with no targets; call addTarget() before use.")
		.def_property(
			"toggleKey",
			&osgx::MultiCameraManipulator::getToggleKey,
			&osgx::MultiCameraManipulator::setToggleKey,
			"Key that cycles to the next target (default 'x')."
		)
		.def_property_readonly("activeIndex", &osgx::MultiCameraManipulator::getActiveIndex, "Index of the currently active target.")
		.def_property_readonly("numTargets", &osgx::MultiCameraManipulator::getNumTargets, "Number of registered targets.")
		.def(
			"addTarget",
			&osgx::MultiCameraManipulator::addTarget,
			"name"_a,
			"manipulator"_a,
			"camera"_a=nullptr,
			"scene"_a=nullptr,
			"setActive"_a=std::function<void(bool)>(),
			"Registers a new target: `manipulator` handles input while it is active. Optional "
			"`camera`/`scene` swap in a dedicated rendered camera/scene graph, and `setActive(bool)` "
			"-- if given - is called on activation and deactivation."
		)
		.def("activate", &osgx::MultiCameraManipulator::activate, "index"_a, "Activates the target at `index`.")
		.def("next", &osgx::MultiCameraManipulator::next, "Activates the next target, wrapping around.")
	;

	// osgx::CameraManipulator<Base> (osgx/Manipulators.hpp) - CRTP mixin merging one-shot/
	// persistent "camera intent" callbacks onto a SINGLE real manipulator instance (as opposed to
	// wrapping/replacing it, see the header's own class comment for why that distinction mattered).
	// Only the TrackballManipulator instantiation is bound for now - the only Base actually
	// verified working (examples/osgx-manipulator.cpp's "intents" mode) - exposed as plain
	// "CameraManipulator" to match osgx::CameraManipulator<>'s own default Base directly.
	auto cameraManipulator = py::class_<
		TrackballCameraManipulator,
		osgGA::CameraManipulator,
		osg::ref_ptr<TrackballCameraManipulator>
	>(
		m,
		"CameraManipulator",
		"osgGA.TrackballManipulator with one-shot/persistent \"camera intent\" callbacks (flyTo, "
		"shake, or any custom osg.Callback) mergeable onto the SAME manipulator instance, rather "
		"than wrapping or replacing it."
	)
		.def(py::init<>(), "Constructs a TrackballManipulator-based manipulator with no camera intents attached.")
		.def(
			"addUpdateCameraCallback",
			&TrackballCameraManipulator::addUpdateCameraCallback,
			"callback"_a,
			"runOnce"_a=false,
			"Attaches an osg::Callback, run(this, camera) every updateCamera(); runOnce=True "
			"auto-removes it once its run() returns False."
		)
		.def(
			"removeUpdateCameraCallback",
			&TrackballCameraManipulator::removeUpdateCameraCallback,
			"callback"_a,
			"Detaches a previously attached update-camera callback."
		)
		.def_property_readonly(
			"currentTime",
			&TrackballCameraManipulator::currentTime,
			"FRAME-event time, cached each frame - what attached intents read for their own timing."
		)
		.def(
			"flyTo",
			[](
				TrackballCameraManipulator& self,
				osg::Vec3d eye,
				osg::Vec3d center,
				osg::Vec3d up,
				double duration,
				std::function<float(float)> ease,
				osgAnimation::Motion::TimeBehaviour tb
			) {
				self.addUpdateCameraCallback(
					new osgx::FlyToCallback({eye, center, up}, duration, std::move(ease), tb),
					true
				);
			},
			"eye"_a,
			"center"_a,
			"up"_a=osg::Vec3d(0.0, 0.0, 1.0),
			"duration"_a,
			"ease"_a=std::function<float(float)>(osgx::defaultEase),
			"tb"_a=osgAnimation::Motion::CLAMP,
			"Convenience wrapper: adds a one-shot FlyToCallback. `ease` is any (float) -> float "
			"callable - pass one of OpenSceneGraph's osgAnimation module's curves "
			"(osgAnimation.inOutCubic, .outBounce, .outElastic, ...) or your own. `tb` is any "
			"osgAnimation.Motion.TimeBehaviour - CLAMP (default) arrives once and stays parked; "
			"LOOP repeats forever (use FlyToCallback directly for a multi-waypoint patrol/loop)."
		)
		.def(
			"shake",
			[](
				TrackballCameraManipulator& self,
				double intensity,
				double duration,
				osgAnimation::Motion::TimeBehaviour tb
			) {
				self.addUpdateCameraCallback(new osgx::ShakeCallback(intensity, duration, tb), true);
			},
			"intensity"_a,
			"duration"_a,
			"tb"_a=osgAnimation::Motion::CLAMP,
			"Convenience wrapper: adds a one-shot ShakeCallback. `tb`=LOOP gives a persistent "
			"repeating rumble instead of a one-shot decay."
		)
	;

	// .callbacks - read-only introspection of what's currently attached (see the
	// pyx::SequenceTraits<TrackballCameraManipulator> specialization above this function).
	pyx::bind_proxy_property<detail::CallbacksProxy, TrackballCameraManipulator, detail::CallbacksStorage>(
		cameraManipulator, "_Callbacks", "callbacks",
		"Read-only introspection of the update-camera callbacks currently attached, as a real Python list."
	);

	py::class_<osgx::Viewpoint>(
		m,
		"Viewpoint",
		"An (eye, center, up) camera pose, as used by FlyToCallback's target/waypoints."
	)
		.def(py::init([](osg::Vec3d eye, osg::Vec3d center, osg::Vec3d up) {
			return osgx::Viewpoint{eye, center, up};
		}), "eye"_a, "center"_a, "up"_a=osg::Vec3d(0.0, 0.0, 1.0), "Constructs a Viewpoint; `up` defaults to +Z.")
		.def_readwrite("eye", &osgx::Viewpoint::eye, "Eye (camera) position.")
		.def_readwrite("center", &osgx::Viewpoint::center, "Look-at target position.")
		.def_readwrite("up", &osgx::Viewpoint::up, "Up vector.")
	;

	// osg::Callback itself is registered by pyosg (OpenSceneGraph.py/pyosg/osg/NodeCallback.cpp),
	// imported before this runs (see osgx.cpp) - FlyToCallback/ShakeCallback derive from it
	// directly (not osg::NodeCallback), matching the (manipulator, camera) object/data pair
	// CameraManipulator<Base>::updateCamera() passes to run(), which osg::NodeCallback's
	// operator()(Node*, NodeVisitor*) convenience doesn't fit.
	py::class_<
		osgx::FlyToCallback,
		osg::Callback,
		osg::ref_ptr<osgx::FlyToCallback>
	>(
		m,
		"FlyToCallback",
		"A one-shot or multi-waypoint camera fly-to animation, driven by a real osgAnimation "
		"Motion/CompositeMotion. Attach via CameraManipulator.flyTo() for the single-target case, "
		"or construct directly (the multi-waypoint constructor) for a scripted patrol/loop."
	)
		.def(
			py::init<
				const osgx::Viewpoint&,
				double,
				std::function<float(float)>,
				osgAnimation::Motion::TimeBehaviour
			>(),
			"target"_a,
			"duration"_a,
			"ease"_a=std::function<float(float)>(osgx::defaultEase),
			"tb"_a=osgAnimation::Motion::CLAMP
		)
		.def(
			py::init<
				std::vector<osgx::Viewpoint>,
				std::vector<double>,
				std::function<float(float)>,
				osgAnimation::Motion::TimeBehaviour
			>(),
			"waypoints"_a,
			"durations"_a,
			"ease"_a=std::function<float(float)>(osgx::defaultEase),
			"tb"_a=osgAnimation::Motion::CLAMP,
			"Multi-waypoint constructor: waypoints.size() == durations.size(), leg i flies from "
			"waypoints[i - 1] (or the camera's current pose, for i == 0) to waypoints[i]. tb=LOOP "
			"repeats the whole path forever - for a smooth ping-pong, author waypoints so the last "
			"and first points coincide or are close (e.g. waypoints=[B, A])."
		)
	;

	py::class_<
		osgx::ShakeCallback,
		osg::Callback,
		osg::ref_ptr<osgx::ShakeCallback>
	>(
		m,
		"ShakeCallback",
		"A camera shake/rumble effect layered on top of the manipulator's own view, driven by a "
		"real osgAnimation Motion. Attach via CameraManipulator.shake() or directly via "
		"addUpdateCameraCallback()."
	)
		.def(
			py::init<double, double, osgAnimation::Motion::TimeBehaviour>(),
			"intensity"_a, "duration"_a, "tb"_a=osgAnimation::Motion::CLAMP,
			"Constructs a shake of `intensity` decaying over `duration` seconds. tb=LOOP gives a "
			"persistent repeating rumble instead of a one-shot decay."
		)
	;

	// osgx::pbr / osgx::ibl - ported from the STATIC path of pyosg-lighting/09-ibl.py and
	// already proven in osgSlug's osgslug-pbr-ibl.cpp; the goal is for Python demos to reuse
	// this toolkit (GLSL snippets + resolveShaderLibs() + the cubemap/BRDF-LUT/SH9 host-side
	// helpers) instead of re-deriving the shader-buffer plumbing from scratch each time.
	m.def(
		"resolveShaderLibs",
		&osgx::resolveShaderLibs,
		"src"_a,
		"Expand registered '#pragma osgx::...' lines into their GLSL source."
	);

	m.def(
		"cachedShader",
		&osgx::cachedShader,
		"type"_a,
		"src"_a,
		"Return the process-wide cached osg.Shader for this (type, source) pair. Repeated calls "
		"with identical arguments return the same shader object, allowing OSG to reuse its compiled "
		"per-context shader."
	);

	// Shader-object substitution hook slots, the counterpart to resolveShaderLibs()' text
	// splicing above - see osgx::applyHooks() (Shader.hpp) and PBRIBLScene.create()'s "hooks"
	// parameter. A HookList is just `[(osgx.Hook.Tonemap, shader), ...]` in Python; no separate
	// binding is needed for HookList itself, pybind11's stl.h vector/pair casters cover it once
	// osgx.Hook is bound below.
	py::enum_<osgx::Hook>(
		m,
		"Hook",
		"Shader-object substitution slots (see resolveShaderLibs()/applyHooks() and "
		"PBRIBLScene.create()'s `hooks` parameter). Each hook REPLACES its default shader object "
		"entirely - GLSL permits one body per function, so attaching a second definition "
		"alongside the built-in is a link error, not an override. Tonemap replaces "
		"osgx_Tonemap(); Skinning replaces osgx_gltf_ApplySkin(); DirectLighting replaces "
		"osgx_DirectLighting(); DeferredLighting replaces the entire fullscreen deferred-lighting shader."
	)
		.value("Tonemap", osgx::Hook::Tonemap)
		.value("Skinning", osgx::Hook::Skinning)
		.value("DeferredLighting", osgx::Hook::DeferredLighting)
		.value("DirectLighting", osgx::Hook::DirectLighting)
	;
}

}
