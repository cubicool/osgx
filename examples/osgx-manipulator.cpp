// vimrun! ./examples/osgx-manipulator
//
// Demonstrates osgx::Ortho2DManipulator (default), osgx::OrbitAxisManipulator ("orbit"), and
// osgx::CameraManipulator<> ("intents").
//
// With no arguments, renders a grid of colored boxes in the XY plane.
// Pass "orbit" to use OrbitAxisManipulator instead of Ortho2DManipulator.
// Pass "intents" to use osgx::CameraManipulator<> (defaults to TrackballManipulator) wrapped with
// one-shot camera intents - press '1' for a FlyToCallback to an alternate viewpoint, '2' for a
// ShakeCallback. Normal trackball orbit/pan/zoom/Home all still work exactly as plain
// TrackballManipulator would, proving Base inheritance is transparent - this is the C++-only
// verification step for osgx::CameraManipulator<Base>, no Python involved.
// Pass a model path (after "orbit"/"intents", if present) to load and inspect it instead of the
// grid.
//
// In "orbit" mode, press 'c' to toggle osgx::CursorCapture: the cursor hides and
// warps back to window-center every frame, feeding accumulated deltas into
// OrbitAxisManipulator::orbitByDelta() instead of the manipulator's own raw-cursor-position
// tracking (which is bounded by the physical screen edge - this is the actual motivating test
// for CursorCapture, see TODO.md's "osgx::platform later work"). The manipulator's own
// MOVE/DRAG-driven orbit is disabled for the duration via setLiveOrbitEnabled(false), since OSG
// delivers every event to both the manipulator and every other GUIEventHandler unconditionally
// (see the comment on setLiveOrbitEnabled() in osgx/Manipulators.hpp for why that matters here).

#include "osgx/CameraIntents.hpp"
#include "osgx/Callbacks.hpp"
#include "osgx/Core.hpp"
#include "osgx/Cursor.hpp"
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
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <iostream>
#include <string>

static osg::ref_ptr<osg::Node> createDefaultScene() {
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

// Bridges osgx::CursorCapture into OrbitAxisManipulator::orbitByDelta(): toggles
// capture on 'c', and while captured, feeds each frame's accumulated pixel delta - normalized by
// the window's half-width/half-height to match orbitByDelta()'s [-1, 1]-ish scale - into the
// manipulator instead of letting it track the raw cursor itself.
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

		osg::Vec2 delta = capture->consume();
		double w = ea.getXmax() - ea.getXmin();
		double h = ea.getYmax() - ea.getYmin();

		// CursorCapture reports raw ea.getX()/getY() units by design (see osgx/Cursor.hpp) - it
		// doesn't know or care what those units mean to a caller. orbitByDelta()'s dy, though,
		// matches getYnormalized() (up-positive), the same convention OrbitAxisManipulator's own
		// live MOVE/DRAG tracking uses internally. Raw Y is up-positive or down-positive depending on
		// the window's mouse orientation (X11 defaults to Y_INCREASING_DOWNWARDS), so it must be
		// flipped to match here - otherwise height responds backwards relative to the uncaptured
		// feel. X has no such flip: getXnormalized() has no orientation dependence.
		double dy = ea.getMouseYOrientation() == osgGA::GUIEventAdapter::Y_INCREASING_DOWNWARDS
			? -delta.y()
			: delta.y()
		;

		if(w > 0.0 && h > 0.0) manip->orbitByDelta(2.0 * delta.x() / w, 2.0 * dy / h);

		return false;
	}

private:
	osg::observer_ptr<osgx::CursorCapture> _capture;
	osg::observer_ptr<osgx::OrbitAxisManipulator> _manip;
};

int main(int argc, char** argv) {
	osgViewer::Viewer viewer;

	bool orbitMode = argc >= 2 && std::string(argv[1]) == "orbit";
	bool intentsMode = argc >= 2 && std::string(argv[1]) == "intents";
	const char* modelPath = (orbitMode || intentsMode)
		? (argc >= 3 ? argv[2] : nullptr)
		: (argc >= 2 ? argv[1] : nullptr)
	;

	osg::ref_ptr<osg::Node> scene;

	if(modelPath) {
		scene = osgDB::readRefNodeFile(modelPath);

		if(!scene) {
			std::cerr << "Failed to load: " << modelPath << std::endl;

			return 1;
		}
	}

	else scene = createDefaultScene();

	viewer.setSceneData(scene);
	viewer.addEventHandler(new osgViewer::StatsHandler());

	if(orbitMode) {
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

	else if(intentsMode) {
		auto manip = osgx::make_ref<osgx::CameraManipulator<>>();

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
			<< "osgx::CameraManipulator<> (TrackballManipulator + camera intents)" << std::endl
			<< " Normal trackball orbit/pan/zoom, Space/Home reset" << std::endl
			<< " '1' FlyToCallback to an alternate viewpoint (1.5s)" << std::endl
			<< " '2' ShakeCallback (0.4s)" << std::endl
			<< " '3' toggle a multi-waypoint FlyToCallback LOOP patrol between two viewpoints" << std::endl
			<< " '1'/'2' both compose cleanly on top of an active '3' patrol - try it" << std::endl
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
