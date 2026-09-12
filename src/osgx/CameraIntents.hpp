#pragma once

#include "Core.hpp"
#include "Manipulators.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Callback>
#include <osg/Camera>
#include <osg/Matrix>
#include <osg/Quat>
#include <osg/Vec3d>
#include <osgAnimation/EaseMotion>
#include <osgGA/CameraManipulator>

OSGX_ENABLE_WARNINGS

#include <functional>
#include <random>
#include <vector>

namespace osgx {

// ================================================================================================
// Built-in camera intents
//
// Plain osg::Callback subclasses (no dependency on any concrete CameraManipulator<Base>
// instantiation), meant to be attached via CameraManipulator<Base>::addUpdateCameraCallback() (see
// osgx/Manipulators.hpp). Both cast their `object` argument to osgx::CameraIntentHost to read
// currentTime() (FRAME-event time, cached by the mixin - not a polled osg::Timer), and their
// `data` argument to osg::Camera to read/write the view matrix currently being composed.
//
// Both drive their timing through real osgAnimation::Motion/CompositeMotion (osgAnimation is
// already a linked osgx dependency, and the same library OpenSceneGraph.py's `osgAnimation` Python
// module exposes) instead of hand-rolled elapsed/duration math - this is what gives both classes a
// genuine osgAnimation::Motion::LOOP mode for free, rather than a bespoke loop primitive.
// ================================================================================================

// `ease` parameters below are a plain (float) -> float callable, not a bespoke enum - pass any of
// osgAnimation::EaseMotion's ~28 curve functions (e.g. `osgAnimation.outElastic` from Python), a
// Motion instance's getValueAt, or a fully custom curve. defaultEase() is what's used when the
// caller doesn't pass one - delegates to InOutCubicFunction, not a hand-rolled approximation.
float defaultEase(float t);

struct Viewpoint {
	osg::Vec3d eye;
	osg::Vec3d center;
	osg::Vec3d up{0.0, 0.0, 1.0};
};

// Animates the camera through a sequence of Viewpoints - the camera's own current pose on the
// first run() is the implicit starting point, so a single-target flight only needs a
// `target`/`duration`. Each leg lerps eye position and slerps orientation (NOT lerping two
// lookAt() "center" points directly, which breaks down when start/target orientations are far
// apart). `ease` is shared across every leg of one FlyToCallback - a caller wanting per-leg
// easing chains multiple instances instead.
//
// Under osgAnimation::Motion::CLAMP (the default), on arrival this writes the EXACT final pose and
// resyncs the manipulator's own state via CameraManipulator::setByMatrix() so control hands back to
// it seamlessly, then goes permanently inert - safe to leave attached with runOnce=false, it will
// NOT keep rewriting the camera and fighting further user input after arrival.
//
// osgAnimation::Motion::LOOP makes the WHOLE path repeat forever - there is no separate loop/
// patrol/ping-pong primitive. This follows the exact convention osg::AnimationPath's own LOOP mode
// already uses: looping just wraps elapsed time back to the start of the path, and a visually
// SEAMLESS loop is the caller's responsibility (author a waypoint list whose last and first points
// coincide or are close - e.g. waypoints={B, A} for a smooth A<->B ping-pong).
//
// KNOWN LIMITATION: setByMatrix() cannot perfectly restore an orbit-style manipulator's pivot/
// distance from a single matrix (osgGA::OrbitManipulator::setByMatrix(), the base of
// TrackballManipulator, reconstructs its center using its OWN stale pre-flight distance - a
// single 4x4 matrix can't encode an orbit pivot). The camera looks correct immediately after
// arrival, but the manipulator's orbit center/distance may be off until something re-homes it --
// the first post-flight drag/zoom on an orbit-style Base may pivot around the wrong point.
//
// Add with runOnce=true for a normal fire-and-forget flight - this callback returns false exactly
// once, on the frame it completes. Under LOOP it never returns false, so runOnce is inert either
// way - something that never finishes can't be "removed on finish".
class FlyToCallback: public osg::Callback {
public:
	// Note: delegates via explicit std::vector<...>{...} construction, not a bare {target}/
	// {duration} braced-init-list - a single-element brace-list whose element type already
	// matches this OTHER constructor's own parameter type is a better (identity) conversion than
	// the vector's initializer_list constructor (a user-defined conversion), so a bare {target}
	// resolves right back to THIS constructor - infinite self-delegation, caught at compile time
	// ("constructor delegates to itself"), not a runtime bug, but worth this comment so nobody
	// "simplifies" it back.
	FlyToCallback(
		const Viewpoint& target,
		double duration,
		std::function<float(float)> ease=defaultEase,
		osgAnimation::Motion::TimeBehaviour tb=osgAnimation::Motion::CLAMP
	): FlyToCallback(
		std::vector<Viewpoint>{target}, std::vector<double>{duration}, std::move(ease), tb
	) {}

	// waypoints.size() must equal durations.size() and be non-empty (throws std::invalid_argument
	// otherwise). Leg i flies from waypoints[i - 1] (or the camera's captured starting pose, for
	// i == 0) to waypoints[i].
	FlyToCallback(
		std::vector<Viewpoint> waypoints,
		std::vector<double> durations,
		std::function<float(float)> ease=defaultEase,
		osgAnimation::Motion::TimeBehaviour tb=osgAnimation::Motion::CLAMP
	);

	bool run(osg::Object* object, osg::Object* data) override;

private:
	struct Leg {
		osg::Vec3d eye;
		osg::Quat orientation;
	};

	Viewpoint _finalTarget;
	osg::ref_ptr<osgAnimation::CompositeMotion> _timeline;
	std::vector<float> _cumulative; // prefix sums of leg durations, for active-leg lookup

	// _legs[0] is the captured starting pose (filled lazily on the first run(), same reasoning as
	// _startTime below); _legs[1..N] are waypoints[0..N-1] resolved to eye/orientation pairs.
	std::vector<Leg> _legs;

	bool _arrived = false;

	// Captured lazily on the FIRST run(), not at construction - avoids reading a stale/zero
	// currentTime() if this is attached before the first real FRAME event of a run.
	double _startTime = -1.0;
};

// Composes decaying rotational jitter (`intensity` being the maximum jitter angle in degrees,
// decaying via a stock osgAnimation::LinearMotion from 1 to 0 across `duration` seconds) on top of
// whatever's already in the camera's view matrix by the time this runs - right-multiplied
// (finalView = baseView * jitterDelta), matching OSG's row-vector convention (see
// Ortho2DManipulator's own comment on multiplication order in src/Manipulators.cpp). The
// manipulator's own internal state is never touched.
//
// osgAnimation::Motion::LOOP (default is CLAMP) makes the decay-and-jitter cycle repeat forever --
// useful for a persistent "idle rumble" effect. Under CLAMP, once decayed, this becomes a permanent
// no-op - safe, if slightly wasteful, to leave attached with runOnce=false; prefer runOnce=true
// for a normal one-shot shake.
class ShakeCallback: public osg::Callback {
public:
	ShakeCallback(
		double intensity,
		double duration,
		osgAnimation::Motion::TimeBehaviour tb=osgAnimation::Motion::CLAMP
	);

	bool run(osg::Object* object, osg::Object* data) override;

private:
	double _intensity;
	osg::ref_ptr<osgAnimation::LinearMotion> _decay;
	bool _finished = false;
	double _startTime = -1.0; // same lazy-capture reasoning as FlyToCallback

	std::mt19937 _rng{std::random_device{}()};
	std::uniform_real_distribution<double> _jitter{-1.0, 1.0};
};

}
