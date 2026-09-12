#include "osgx/CameraIntents.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Math>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <stdexcept>

namespace osgx {

namespace {

// Minimum leg/path duration - osgAnimation::Motion::getValueAt() divides by duration with no
// guard of its own; a caller-supplied 0.0 (or negative) would silently produce NaN through the
// ease function instead of a crash or a warning, corrupting the camera matrix. Floor it instead.
constexpr float MIN_DURATION = 1e-6f;

// Adapts an arbitrary std::function<float(float)> ease curve into a real osgAnimation::Motion leaf
// so it can be a CompositeMotion child alongside stock Motion types. Always CLAMP internally --
// only the outer path timeline (FlyToCallback::_timeline) should ever see LOOP; looping a leg's own
// local progress would double-wrap and corrupt CompositeMotion's leg-selection math.
class LambdaMotion: public osgAnimation::Motion {
public:
	LambdaMotion(std::function<float(float)> ease, float duration):
	osgAnimation::Motion(0.0f, duration, 1.0f, osgAnimation::Motion::CLAMP),
	_ease(std::move(ease)) {}

	void getValueInNormalizedRange(float t, value_type& result) const override {
		result = _ease(t);
	}

private:
	std::function<float(float)> _ease;
};

}

float defaultEase(float t) {
	float result = 0.0f;

	osgAnimation::InOutCubicFunction::getValueAt(t, result);

	return result;
}

FlyToCallback::FlyToCallback(
	std::vector<Viewpoint> waypoints,
	std::vector<double> durations,
	std::function<float(float)> ease,
	osgAnimation::Motion::TimeBehaviour tb
) {
	if(waypoints.empty() || waypoints.size() != durations.size()) {
		throw std::invalid_argument(
			"FlyToCallback: waypoints/durations must be equal-sized and non-empty"
		);
	}

	_finalTarget = waypoints.back();
	_legs.resize(waypoints.size() + 1); // _legs[0] captured lazily on the first run()

	std::vector<float> legDurations(waypoints.size());
	float total = 0.0f;

	for(size_t i = 0; i < waypoints.size(); i++) {
		const Viewpoint& wp = waypoints[i];
		osg::Matrixd cameraToWorld = osg::Matrixd::inverse(
			osg::Matrixd::lookAt(wp.eye, wp.center, wp.up)
		);

		_legs[i + 1].eye = cameraToWorld.getTrans();
		_legs[i + 1].orientation = cameraToWorld.getRotate();

		legDurations[i] = std::max(static_cast<float>(durations[i]), MIN_DURATION);
		total += legDurations[i];

		_cumulative.push_back(total);
	}

	// CompositeMotion does NOT auto-sum its children's durations into its own - its leg-selection
	// walk normalizes against whatever duration it was constructed with, so `total` must be passed
	// explicitly here or that walk silently breaks.
	_timeline = new osgAnimation::CompositeMotion(0.0f, total, 1.0f, tb);

	for(size_t i = 0; i < waypoints.size(); i++) {
		_timeline->getMotionList().push_back(new LambdaMotion(ease, legDurations[i]));
	}
}

bool FlyToCallback::run(osg::Object* object, osg::Object* data) {
	if(_arrived) return false; // never re-touch _timeline once CLAMP has pinned it - see below

	auto* camera = dynamic_cast<osg::Camera*>(data);
	auto* host = dynamic_cast<CameraIntentHost*>(object);

	if(!camera || !host) return false;

	double now = host->currentTime();

	if(_startTime < 0.0) {
		osg::Matrixd startCameraToWorld = osg::Matrixd::inverse(camera->getViewMatrix());

		_legs[0].eye = startCameraToWorld.getTrans();
		_legs[0].orientation = startCameraToWorld.getRotate();
		_startTime = now;
	}

	// Absolute elapsed time, not an accumulated per-frame delta - Motion::setTime() re-derives the
	// CLAMP/LOOP-adjusted time from scratch each call, so there's no delta-accumulation drift to
	// worry about. `t` (not raw elapsed) is what everything below must key off: under LOOP, elapsed
	// grows unboundedly while t wraps back into [0, duration) every cycle.
	_timeline->setTime(static_cast<float>(now - _startTime));

	float t = _timeline->getTime();

	// At the exact instant CLAMP first pins t at the path's duration, CompositeMotion's own leg
	// walk (a strict `<` test against each child's duration fraction) falls through every child --
	// even the last one - and hits its "did not find the value in range" WARN + result=0 fallback.
	// Arrival MUST be checked before calling getValue()/doing the leg lookup below, and _timeline
	// must never be touched again once _arrived is set.
	if(t >= _timeline->getDuration()) {
		osg::Matrixd targetView = osg::Matrixd::lookAt(
			_finalTarget.eye, _finalTarget.center, _finalTarget.up
		);

		camera->setViewMatrix(targetView);

		// Resync the manipulator's own state so control hands back to it seamlessly. `this` IS the
		// real manipulator via CameraManipulator<Base>'s inheritance - no separate "inner" object.
		// See the KNOWN LIMITATION comment in CameraIntents.hpp: this can't perfectly restore an
		// orbit-style manipulator's pivot/distance from a single matrix.
		if(auto* manip = dynamic_cast<osgGA::CameraManipulator*>(object)) {
			manip->setByMatrix(osg::Matrixd::inverse(targetView));
		}

		_arrived = true;

		return false;
	}

	// upper_bound, NOT lower_bound: CompositeMotion selects children with a strict `t <
	// durationInRange` test, i.e. at an exact leg-boundary instant it picks the NEXT leg (local
	// t == 0), not the one that just ended - upper_bound (first cumulative sum strictly greater
	// than t) matches that; lower_bound would disagree by one leg at exact boundaries.
	size_t activeLeg = std::min(
		static_cast<size_t>(
			std::upper_bound(_cumulative.begin(), _cumulative.end(), t) - _cumulative.begin()
		),
		_legs.size() - 2
	);

	double eased = static_cast<double>(_timeline->getValue());
	const Leg& from = _legs[activeLeg];
	const Leg& to = _legs[activeLeg + 1];
	osg::Vec3d eye = from.eye + (to.eye - from.eye) * eased;
	osg::Quat orientation;

	orientation.slerp(eased, from.orientation, to.orientation);

	osg::Matrixd cameraToWorld = osg::Matrixd::rotate(orientation) * osg::Matrixd::translate(eye);

	camera->setViewMatrix(osg::Matrixd::inverse(cameraToWorld));

	return true;
}

ShakeCallback::ShakeCallback(
	double intensity,
	double duration,
	osgAnimation::Motion::TimeBehaviour tb
):
_intensity(intensity),
_decay(new osgAnimation::LinearMotion(
	1.0f, std::max(static_cast<float>(duration), MIN_DURATION), -1.0f, tb
)) {}

bool ShakeCallback::run(osg::Object* object, osg::Object* data) {
	if(_finished) return false;

	auto* camera = dynamic_cast<osg::Camera*>(data);
	auto* host = dynamic_cast<CameraIntentHost*>(object);

	if(!camera || !host) return false;

	double now = host->currentTime();

	if(_startTime < 0.0) _startTime = now;

	_decay->setTime(static_cast<float>(now - _startTime));

	if(_decay->getTime() >= _decay->getDuration()) {
		_finished = true;

		return false;
	}

	double decay = static_cast<double>(_decay->getValue());
	double angleDeg = _intensity * decay;

	osg::Quat jitter(
		osg::DegreesToRadians(angleDeg * _jitter(_rng)), osg::Vec3d(1.0, 0.0, 0.0),
		osg::DegreesToRadians(angleDeg * _jitter(_rng)), osg::Vec3d(0.0, 1.0, 0.0),
		osg::DegreesToRadians(angleDeg * _jitter(_rng)), osg::Vec3d(0.0, 0.0, 1.0)
	);

	camera->setViewMatrix(camera->getViewMatrix() * osg::Matrixd::rotate(jitter));

	return true;
}

}
