// vimrun! ./examples/osgx-manipulator --type ortho2d
//
// osgx-manipulator --type <TYPE> [--invert-y 0|1] [--invert-zoom 0|1] [modelPath]
//
// <TYPE> is one of: ortho2d (default), orbit, actions, firstperson, thirdperson.
// Pass a model path (any position) to load and inspect it instead of the default scene.
// --invert-y 0|1 (default 1) applies to firstperson/thirdperson only - see
// osgx::PlayerManipulator<Base>::setInvertY() in osgx/Manipulators.hpp. Deliberately set
// explicitly here rather than left as the library's own default, so this example is the place to
// compare both feels rather than that decision living silently inside the library.
// --invert-zoom 0|1 (default 0) applies to thirdperson only - see
// osgx::PlayerFollowManipulator::setInvertZoom() in osgx/Manipulators.hpp. Same reasoning as
// --invert-y above: set explicitly here, not baked into the library's own default.
//
// ortho2d      osgx::Ortho2DManipulator - pan/zoom/Ctrl+drag-tilt (see its own class comment)
// orbit        osgx::OrbitAxisManipulator - turntable orbit/height/zoom; 'c' toggles
//              osgx::CursorCapture (hide+warp+accumulate, feeding orbitByDelta() instead of the
//              manipulator's own raw-cursor-position tracking, which is bounded by the physical
//              screen edge - this is the actual motivating test for CursorCapture, see TODO.md's
//              "osgx::platform later work"). The manipulator's own MOVE/DRAG-driven orbit is
//              disabled for the duration via setLiveOrbitEnabled(false), since OSG delivers every
//              event to both the manipulator and every other GUIEventHandler unconditionally (see
//              the comment on setLiveOrbitEnabled() in osgx/Manipulators.hpp for why that matters)
// actions      osgx::ActionsManipulator<> (defaults to TrackballManipulator) wrapped with one-shot
//              camera actions - '1' FlyToCallback, '2' ShakeCallback, '3' a LOOP patrol. Normal
//              trackball orbit/pan/zoom/Home all still work exactly as plain TrackballManipulator
//              would, proving Base inheritance is transparent - the C++-only verification step for
//              osgx::ActionsManipulator<Base>, no Python involved.
// firstperson  osgx::FirstPersonManipulator - WASD + mouse look; 'l' toggles LookStyle between
//              ALWAYS (raw mouse look, no button) and CLICK_HOLD (look only while the configured
//              look button - RIGHT by default - is held and dragging). The cursor is hidden and
//              captured (hide+warp+accumulate) automatically - no manual osgx::CursorCapture
//              wiring needed here, unlike "orbit" below - see osgx::PlayerManipulator<Base>'s own
//              class comment in Manipulators.hpp for why.
// thirdperson  osgx::ThirdPersonManipulator - orbits a "target" marker (added to the default scene
//              only; ignored when a model path is given) at a configurable distance, proving
//              osgx::PlayerFollowManipulator keeps the camera's azimuth locked to the target's
//              own facing instead of drifting independently. W/A/S/D drive the target itself via
//              osgx::PlayerMovementHandler (TANK style - W/S move along its current facing, A/D
//              turn in place, no strafe); left-drag still orbits the camera manually on top of
//              that. The cursor is captured automatically for the duration of the drag, same as
//              firstperson above.

#include "osgx/CameraActions.hpp"
#include "osgx/Callbacks.hpp"
#include "osgx/Core.hpp"
#include "osgx/Cursor.hpp"
#include "osgx/Library.hpp"
#include "osgx/Manipulators.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osgDB/ReadFile>
#include <osgGA/GUIEventHandler>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <iostream>
#include <string>

static osg::ref_ptr<osg::Group> createDefaultScene() {
	auto root = osgx::make_ref<osg::Group>();

	struct Entry {
		float x, y;

		osg::Vec4 color;
	};

	static const Entry ENTRIES[] = {
		{ 0.0f, 0.0f, { 1.0f, 0.3f, 0.3f, 1.0f } },
		{ 2.0f, 0.0f, { 0.3f, 1.0f, 0.3f, 1.0f } },
		{ -2.0f, 0.0f, { 0.3f, 0.3f, 1.0f, 1.0f } },
		{ 0.0f, 2.0f, { 1.0f, 1.0f, 0.3f, 1.0f } },
		{ 0.0f, -2.0f, { 0.3f, 1.0f, 1.0f, 1.0f } },
		{ 2.0f, 2.0f, { 1.0f, 0.3f, 1.0f, 1.0f } },
		{ -2.0f, -2.0f, { 0.8f, 0.8f, 0.8f, 1.0f } },
		{ 4.0f, 0.0f, { 1.0f, 0.6f, 0.1f, 1.0f } },
		{ -4.0f, 0.0f, { 0.1f, 0.6f, 1.0f, 1.0f } },
	};

	for(const auto& e : ENTRIES) {
		auto xf = osgx::make_ref<osg::MatrixTransform>(osg::Matrix::translate(e.x, e.y, 0.0f));
		auto geode = osgx::make_ref<osg::Geode>();
		auto box = osgx::make_ref<osg::ShapeDrawable>(new osg::Box(osg::Vec3(), 0.8f, 0.8f, 0.1f));

		box->setColor(e.color);
		geode->addDrawable(box);
		xf->addChild(geode);
		root->addChild(xf);
	}

	return root;
}

// Adds a small "target" (body box + an offset facing marker, so its azimuth is visible at a
// glance) to `root`, resting on the floor at a fixed height - osgx::PlayerMovementHandler (added
// by the caller, see the thirdperson branch below) owns the transform's matrix from then on,
// driving it via WASD.
//
// No separate "anchor" node needed (an earlier version of this function added one specifically to
// work around osgGA::NodeTrackerManipulator tracking a node's getBound().center() instead of its
// local origin - that bug, and the workaround, are both gone now that
// osgx::ThirdPersonManipulator is built on osgx::PlayerFollowManipulator, which tracks a node's
// live world-space TRANSLATION directly and never touches its bound at all). `target` itself is
// what ThirdPersonManipulator::setTrackNode() wants.
static osg::ref_ptr<osg::MatrixTransform> addTarget(osg::Group* root) {
	auto target = osgx::make_ref<osg::MatrixTransform>();
	auto body = osgx::make_ref<osg::Geode>();
	auto bodyShape = osgx::make_ref<osg::ShapeDrawable>(new osg::Box(osg::Vec3(), 0.6f, 0.6f, 1.2f));

	bodyShape->setColor(osg::Vec4(0.9f, 0.9f, 0.2f, 1.0f));
	body->addDrawable(bodyShape);

	auto facing = osgx::make_ref<osg::Geode>();
	auto facingShape = osgx::make_ref<osg::ShapeDrawable>(
		new osg::Box(osg::Vec3(0.0f, 0.5f, 0.3f), 0.2f, 0.2f, 0.2f)
	);

	facingShape->setColor(osg::Vec4(1.0f, 0.1f, 0.1f, 1.0f));
	facing->addDrawable(facingShape);

	target->addChild(body);
	target->addChild(facing);
	target->setMatrix(osg::Matrix::translate(0.0, 0.0, 0.6));

	root->addChild(target);

	return target;
}

// Bridges osgx::CursorCapture into OrbitAxisManipulator::orbitByDelta(): toggles capture on 'c',
// and while captured, feeds each frame's accumulated delta - via drainNormalized(), which
// already matches orbitByDelta()'s [-1, 1]-ish scale (see CursorCapture's own class comment in
// Cursor.hpp for why that normalization can't just be skipped) - into the manipulator instead of
// letting it track the raw cursor itself.
class OrbitCaptureBridge: public osgGA::GUIEventHandler {
public:
	OrbitCaptureBridge(osgx::CursorCapture* capture, osgx::OrbitAxisManipulator* manip):
	_capture(capture), _manip(manip) {}

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override {
		if(ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN && ea.getKey() == 'c') {
			auto* capture = _capture.get();
			auto* manip = _manip.get();

			if(!capture || !manip) return false;

			bool captured = !capture->isCaptured();

			capture->setCaptured(captured);
			manip->setLiveOrbitEnabled(!captured);

			std::cout << "CursorCapture: " << (captured ? "ON" : "OFF") << std::endl;

			return true;
		}

		if(ea.getEventType() != osgGA::GUIEventAdapter::FRAME) return false;

		auto* capture = _capture.get();
		auto* manip = _manip.get();

		if(!capture || !manip || !capture->isCaptured()) return false;

		osg::Vec2 delta = capture->drainNormalized(ea);

		manip->orbitByDelta(delta.x(), delta.y());

		return false;
	}

private:
	osg::observer_ptr<osgx::CursorCapture> _capture;
	osg::observer_ptr<osgx::OrbitAxisManipulator> _manip;
};

enum class ManipulatorType { ORTHO2D, ORBIT, ACTIONS, FIRSTPERSON, THIRDPERSON };

static bool parseType(const std::string& s, ManipulatorType& type) {
	if(s == "ortho2d") type = ManipulatorType::ORTHO2D;
	else if(s == "orbit") type = ManipulatorType::ORBIT;
	else if(s == "actions") type = ManipulatorType::ACTIONS;
	else if(s == "firstperson") type = ManipulatorType::FIRSTPERSON;
	else if(s == "thirdperson") type = ManipulatorType::THIRDPERSON;
	else return false;

	return true;
}

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	osgViewer::Viewer viewer;

	ManipulatorType type = ManipulatorType::ORTHO2D;
	const char* modelPath = nullptr;
	bool invertY = true; // applied explicitly below for firstperson/thirdperson - see --invert-y
	bool invertZoom = false; // thirdperson only - see --invert-zoom

	for(int i = 1; i < argc; i++) {
		if(std::string(argv[i]) == "--type") {
			if(i + 1 >= argc) {
				std::cerr << "--type requires a value" << std::endl;

				return 1;
			}

			if(!parseType(argv[++i], type)) {
				std::cerr << "Unknown --type: " << argv[i] << std::endl;

				return 1;
			}
		}

		else if(std::string(argv[i]) == "--invert-y") {
			if(i + 1 >= argc || (std::string(argv[i + 1]) != "0" && std::string(argv[i + 1]) != "1")) {
				std::cerr << "--invert-y requires a value of 0 or 1" << std::endl;

				return 1;
			}

			invertY = std::string(argv[++i]) == "1";
		}

		else if(std::string(argv[i]) == "--invert-zoom") {
			if(i + 1 >= argc || (std::string(argv[i + 1]) != "0" && std::string(argv[i + 1]) != "1")) {
				std::cerr << "--invert-zoom requires a value of 0 or 1" << std::endl;

				return 1;
			}

			invertZoom = std::string(argv[++i]) == "1";
		}

		else modelPath = argv[i];
	}

	osg::ref_ptr<osg::Group> scene;

	if(modelPath) {
		osg::ref_ptr<osg::Node> loaded = osgDB::readRefNodeFile(modelPath);

		if(!loaded) {
			std::cerr << "Failed to load: " << modelPath << std::endl;

			return 1;
		}

		scene = osgx::make_ref<osg::Group>();

		scene->addChild(loaded);
	}

	else scene = createDefaultScene();

	// thirdperson needs something to track - the demo target itself, when there's no explicit
	// model (see the file header comment); otherwise the loaded model's own root, which at least
	// proves orbit-follow on an arbitrary node, just without WASD movement (addTarget()'s
	// PlayerMovementHandler pairing below only applies to the demo target, not an arbitrary loaded
	// model). `trackTarget` and `target` are the SAME node in the demo-target case - kept as two
	// variables only because `trackTarget` also needs to hold an arbitrary loaded-model Node in
	// the other case, where there's no MatrixTransform for PlayerMovementHandler to drive at all.
	osg::ref_ptr<osg::Node> trackTarget;
	osg::ref_ptr<osg::MatrixTransform> target;

	if(type == ManipulatorType::THIRDPERSON) {
		if(modelPath) trackTarget = scene->getChild(0);

		else {
			target = addTarget(scene);
			trackTarget = target;
		}
	}

	viewer.setSceneData(scene);

	if(type == ManipulatorType::ORBIT) {
		auto manip = osgx::make_ref<osgx::OrbitAxisManipulator>();

		viewer.setCameraManipulator(manip);

		auto capture = osgx::make_ref<osgx::CursorCapture>(viewer);

		viewer.addEventHandler(capture);
		viewer.addEventHandler(new OrbitCaptureBridge(capture, manip));

		std::cout
			<< "OrbitAxisManipulator" << std::endl
			<< " Mouse move/drag orbit (X) + height (Y), always active" << std::endl
			<< " Scroll dolly zoom (clamped to model coverage)" << std::endl
			<< " Space/Home reset view" << std::endl
			<< " 'c' toggle CursorCapture (hide+warp+accumulate; unbounded orbit/height)" << std::endl
		;
	}

	else if(type == ManipulatorType::ACTIONS) {
		auto manip = osgx::make_ref<osgx::ActionsManipulator<>>();

		viewer.setCameraManipulator(manip);

		osg::BoundingSphere bs = scene->getBound();
		osgx::Viewpoint flyTarget{
			osg::Vec3d(bs.center()) + osg::Vec3d(0.0, -bs.radius() * 2.5, bs.radius() * 1.5),
			osg::Vec3d(bs.center()),
			osg::Vec3d(0.0, 0.0, 1.0)
		};

		// Two viewpoints swept ~80 degrees apart across the SAME side of the model (not diametrically
		// opposite) - '3' patrols between them forever via a single multi-waypoint FlyToCallback
		// under osgAnimation::Motion::LOOP, no hand-rolled C++ ping-pong driver needed.
		// FlyToCallback's orientation interpolation is a plain slerp between two fixed lookAt()
		// quaternions, not an arc/orbit - two viewpoints ~180 degrees apart (e.g. directly opposite
		// sides, both looking at the same center) makes that slerp degenerate: the rotation angle
		// being interpolated is maximal/near-ambiguous, so partway through the flight the camera can
		// face some arbitrary perpendicular direction, losing the model out of the view frustum
		// entirely instead of smoothly sweeping past it. Keeping both waypoints within well under
		// 180 degrees of each other keeps the model in frame throughout the whole flight.
		//
		// The camera's own current pose becomes the implicit leg-0 start, and LOOP wraps the WHOLE
		// path (including that captured start) back to t=0 every cycle - per FlyToCallback's own
		// documented convention (matching osg::AnimationPath's LOOP), a seamless loop is the caller's
		// job. Pressing '3' from near patrolA/patrolB keeps that wrap unnoticeable in practice;
		// pressing it from an arbitrary orbit position will visibly snap back through that starting
		// pose once per cycle - expected, not a bug.
		osgx::Viewpoint patrolA{
			osg::Vec3d(bs.center()) + osg::Vec3d(bs.radius() * 2.0, -bs.radius() * 1.8, bs.radius() * 0.5),
			osg::Vec3d(bs.center()),
			osg::Vec3d(0.0, 0.0, 1.0)
		};
		osgx::Viewpoint patrolB{
			osg::Vec3d(bs.center()) + osg::Vec3d(bs.radius() * 2.0, bs.radius() * 1.8, bs.radius() * 0.5),
			osg::Vec3d(bs.center()),
			osg::Vec3d(0.0, 0.0, 1.0)
		};

		// Tracks the currently-running patrol (if any) so '3' TOGGLES it - press once to start,
		// press again to stop - rather than stacking a new LOOP FlyToCallback (which never
		// finishes on its own) on every press. Captured by value + mutable: the LambdaKeyHandler
		// owns one persistent copy of this lambda, so the ref_ptr genuinely persists across calls.
		osg::ref_ptr<osgx::FlyToCallback> patrol;
		osg::Camera* camera = viewer.getCamera();

		viewer.addEventHandler(new osgx::LambdaKeyHandler(
			{'1', '2', '3'},
			[manip, camera, flyTarget, patrolA, patrolB, patrol](
				const osgGA::GUIEventAdapter&,
				osgGA::GUIActionAdapter&,
				osgx::LambdaKeyHandler::Key key
			) mutable {
				if(key == '1') {
					manip->addUpdateCameraCallback(new osgx::FlyToCallback(flyTarget, 1.5), true);
				}

				else if(key == '2') {
					manip->addUpdateCameraCallback(new osgx::ShakeCallback(3.0, 0.4), true);
				}

				else if(key == '3') {
					if(patrol.valid()) {
						manip->removeUpdateCameraCallback(patrol);

						// While active, the patrol overwrote the camera's view matrix every frame --
						// Base::updateCamera() still ran first each frame though, so the underlying
						// TrackballManipulator kept silently accumulating from any mouse input that
						// happened to arrive meanwhile (Base::handle() is never intercepted, only
						// peeked at). Without this resync, removing the patrol would snap the view to
						// that stale hidden state instead of resuming smoothly from wherever the
						// patrol left the camera - same resync FlyToCallback already does on normal
						// CLAMP arrival, just triggered by an interrupt instead of completion.
						manip->setByMatrix(osg::Matrixd::inverse(camera->getViewMatrix()));

						patrol = nullptr;
					}

					else {
						patrol = new osgx::FlyToCallback(
							{patrolA, patrolB},
							{2.0, 2.0},
							osgx::defaultEase,
							osgAnimation::Motion::LOOP
						);

						// LOOP never finishes on its own - runOnce is inert here either way; the
						// toggle above is what actually stops it.
						manip->addUpdateCameraCallback(patrol, false);
					}
				}

				return true;
			}
		));

		std::cout
			<< "osgx::ActionsManipulator<> (TrackballManipulator + camera actions)" << std::endl
			<< " Normal trackball orbit/pan/zoom, Space/Home reset" << std::endl
			<< " '1' FlyToCallback to an alternate viewpoint (1.5s)" << std::endl
			<< " '2' ShakeCallback (0.4s)" << std::endl
			<< " '3' toggle a multi-waypoint FlyToCallback LOOP patrol between two viewpoints" << std::endl
			<< " '1'/'2' both compose cleanly on top of an active '3' patrol - try it" << std::endl
		;
	}

	else if(type == ManipulatorType::FIRSTPERSON) {
		auto manip = osgx::make_ref<osgx::FirstPersonManipulator>();

		manip->setInvertY(invertY);
		viewer.setCameraManipulator(manip);

		viewer.addEventHandler(new osgx::LambdaKeyHandler(
			'l',
			[manip](const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&) {
				using LookStyle = osgx::FirstPersonManipulator::LookStyle;

				LookStyle style = manip->getLookStyle() == LookStyle::ALWAYS
					? LookStyle::CLICK_HOLD
					: LookStyle::ALWAYS
				;

				manip->setLookStyle(style);

				std::cout
					<< "LookStyle: " << (style == LookStyle::ALWAYS ? "ALWAYS" : "CLICK_HOLD")
					<< std::endl
				;

				return true;
			}
		));

		std::cout
			<< "FirstPersonManipulator" << std::endl
			<< " W/A/S/D move forward/back/strafe left/right" << std::endl
			<< " Mouse look (LookStyle::ALWAYS by default - no button needed)" << std::endl
			<< " 'l' toggle LookStyle::ALWAYS <-> CLICK_HOLD (getLookButton() held, RIGHT by default)" << std::endl
			<< " Scroll step forward/back, Space/Home reset" << std::endl
		;
	}

	else if(type == ManipulatorType::THIRDPERSON) {
		auto manip = osgx::make_ref<osgx::ThirdPersonManipulator>();

		manip->setTrackNode(trackTarget);
		manip->setInvertY(invertY);
		manip->setInvertZoom(invertZoom);
		viewer.setCameraManipulator(manip);

		if(target.valid()) viewer.addEventHandler(osgx::make_ref<osgx::PlayerMovementHandler>(target.get()));

		std::cout
			<< "ThirdPersonManipulator" << std::endl
			<< " W/S move the target forward/back, A/D turn it in place (TANK style, no strafe)" << std::endl
			<< " Left drag orbit (tracks the target's own azimuth when not dragging)" << std::endl
			<< " Scroll dolly zoom (no pan - PlayerFollowManipulator doesn't implement one)" << std::endl
			<< " Space/Home reset view" << std::endl
		;
	}

	else {
		auto manip = osgx::make_ref<osgx::Ortho2DManipulator>();

		// manip->setPixelNudge(0.5);

		viewer.setCameraManipulator(manip);

		std::cout
			<< "Ortho2DManipulator" << std::endl
			<< " Left drag pan" << std::endl
			<< " Scroll geometric zoom" << std::endl
			<< " Shift+Scroll pixel-nudge zoom (8px/click)" << std::endl
			<< " Ctrl+Left drag 3D pitch/yaw" << std::endl
			<< " Space/Home reset view" << std::endl
		;
	}

	return viewer.run();
}
