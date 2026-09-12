#include "osgx/Manipulators.hpp"

namespace osgx {

namespace {

bool normalize(osg::Vec3d& v) {
	if(v.length2() <= 1e-20) return false;

	v.normalize();

	return true;
}

// Returns a stable unit vector perpendicular to axis.
osg::Vec3d perpendicularTo(const osg::Vec3d& axis) {
	osg::Vec3d reference = std::abs(axis.z()) < 0.9
		? osg::Vec3d(0.0, 0.0, 1.0)
		: osg::Vec3d(0.0, 1.0, 0.0)
	;
	osg::Vec3d result = reference ^ axis;
	normalize(result);

	return result;
}

}

void MultiCameraManipulator::addTarget(
	const std::string& name,
	osgGA::CameraManipulator* manipulator,
	osg::Camera* camera,
	osg::Node* scene,
	std::function<void(bool)> setActive
) {
	Target target;
	target.name = name;
	target.manipulator = manipulator;
	target.camera = camera;
	target.scene = scene;
	target.setActive = setActive;

	if(target.manipulator.valid()) {
		target.manipulator->setNode(scene ? scene : _defaultScene.get());
	}

	_targets.push_back(target);

	if(_targets.size() == 1) activate(0);
}

void MultiCameraManipulator::activate(unsigned int index) {
	if(index >= _targets.size()) return;

	if(_hasActive && _active == index) return;

	if(_hasActive && _active < _targets.size() && _targets[_active].setActive) {
		_targets[_active].setActive(false);
	}

	_active = index;
	_hasActive = true;

	auto& target = _targets[_active];

	if(target.manipulator.valid()) {
		target.manipulator->finishAnimation();
		target.manipulator->setNode(target.scene.valid() ? target.scene.get() : _defaultScene.get());

		if(target.camera.valid()) target.manipulator->setByInverseMatrix(target.camera->getViewMatrix());
	}

	if(target.setActive) target.setActive(true);

	OSG_NOTICE << "MultiCameraManipulator: " << target.name << std::endl;
}

void MultiCameraManipulator::setByMatrix(const osg::Matrixd& matrix) {
	if(auto* target = activeTarget(); target && target->manipulator.valid()) {
		target->manipulator->setByMatrix(matrix);
	}
}

void MultiCameraManipulator::setByInverseMatrix(const osg::Matrixd& matrix) {
	if(auto* target = activeTarget(); target && target->manipulator.valid()) {
		target->manipulator->setByInverseMatrix(matrix);
	}
}

osg::Matrixd MultiCameraManipulator::getMatrix() const {
	auto* target = activeTarget();

	return target && target->manipulator.valid() ? target->manipulator->getMatrix() : osg::Matrixd();
}

osg::Matrixd MultiCameraManipulator::getInverseMatrix() const {
	auto* target = activeTarget();

	return target && target->manipulator.valid() ? target->manipulator->getInverseMatrix() : osg::Matrixd();
}

void MultiCameraManipulator::updateCamera(osg::Camera& mainCamera) {
	auto* target = activeTarget();

	if(!target || !target->manipulator.valid()) return;

	_updateCamera(mainCamera);

	if(target->camera.valid()) target->manipulator->updateCamera(*target->camera);
	else target->manipulator->updateCamera(mainCamera);
}

void MultiCameraManipulator::setNode(osg::Node* node) {
	_defaultScene = node;

	for(auto& target : _targets) {
		if(target.manipulator.valid()) {
			target.manipulator->setNode(target.scene.valid() ? target.scene.get() : _defaultScene.get());
		}
	}
}

osg::Node* MultiCameraManipulator::getNode() {
	auto* target = activeTarget();

	return target && target->manipulator.valid() ? target->manipulator->getNode() : nullptr;
}

const osg::Node* MultiCameraManipulator::getNode() const {
	auto* target = activeTarget();

	return target && target->manipulator.valid() ? target->manipulator->getNode() : nullptr;
}

void MultiCameraManipulator::home(double currentTime) {
	if(auto* target = activeTarget(); target && target->manipulator.valid()) {
		target->manipulator->home(currentTime);
	}
}

void MultiCameraManipulator::home(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) {
	if(auto* target = activeTarget(); target && target->manipulator.valid()) {
		target->manipulator->home(ea, aa);
	}
}

void MultiCameraManipulator::init(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) {
	if(auto* target = activeTarget(); target && target->manipulator.valid()) {
		target->manipulator->init(ea, aa);
	}
}

bool MultiCameraManipulator::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) {
	if(ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN && ea.getKey() == _toggleKey) {
		if(auto* target = activeTarget(); target && target->manipulator.valid()) {
			target->manipulator->init(ea, aa);
		}

		next();

		if(auto* target = activeTarget(); target && target->manipulator.valid()) {
			target->manipulator->init(ea, aa);
		}

		return true;
	}

	auto* target = activeTarget();

	return target && target->manipulator.valid() && target->manipulator->handle(ea, aa);
}

void MultiCameraManipulator::_updateCamera(osg::Camera& mainCamera) {
	if(_mainCameraUpdated) return;

	_mainCameraUpdated = true;

	for(auto& candidate : _targets) {
		if(!candidate.camera.valid() && candidate.manipulator.valid()) {
			candidate.manipulator->finishAnimation();
			candidate.manipulator->setNode(candidate.scene.valid() ? candidate.scene.get() : _defaultScene.get());
			candidate.manipulator->home(0.0);
			candidate.manipulator->updateCamera(mainCamera);

			break;
		}
	}
}

// Extract pan center from the translation component of the camera-to-world matrix.
void Ortho2DManipulator::setPlaneNormal(const osg::Vec3d& normal) {
	osg::Vec3d planeNormal = normal;

	if(!normalize(planeNormal)) return;

	osg::Vec3d screenUp = _screenUp - planeNormal * (_screenUp * planeNormal);

	if(!normalize(screenUp)) screenUp = perpendicularTo(planeNormal);

	_planeNormal = planeNormal;
	_screenUp = screenUp;
}

void Ortho2DManipulator::setScreenUp(const osg::Vec3d& up) {
	osg::Vec3d screenUp = up - _planeNormal * (up * _planeNormal);

	if(!normalize(screenUp)) return;

	_screenUp = screenUp;
}

void Ortho2DManipulator::setByMatrix(const osg::Matrixd& m) {
	_center.set(m(3, 0), m(3, 1), m(3, 2));
}

void Ortho2DManipulator::setByInverseMatrix(const osg::Matrixd& m) {
	setByMatrix(osg::Matrixd::inverse(m));
}

osg::Matrixd Ortho2DManipulator::getMatrix() const {
	return osg::Matrixd::inverse(getInverseMatrix());
}

// World-to-camera (view matrix). _rotation is expressed in the configured plane's local frame.
osg::Matrixd Ortho2DManipulator::getInverseMatrix() const {
	osg::Quat inverseRotation = _rotation.conj();
	osg::Vec3d eye = _center + _toWorld(inverseRotation * osg::Vec3d(0.0, 0.0, 1.0));
	osg::Vec3d up = _toWorld(inverseRotation * osg::Vec3d(0.0, 1.0, 0.0));

	return osg::Matrixd::lookAt(eye, _center, up);
}

void Ortho2DManipulator::updateCamera(osg::Camera& cam) {
	cam.setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
	cam.setViewMatrix(getInverseMatrix());

	const auto* vp = cam.getViewport();
	double aspect = (vp && vp->height() > 0.0)
		? vp->width() / vp->height()
		: 1.0
	;

	double h = _halfExtentY;
	double nearPlane = -1e6;
	double farPlane = 1e6;

	if(_node.valid()) {
		osg::BoundingSphere bs = _node->getBound();

		if(bs.radius() > 0.0) {
			osg::Quat inverseRotation = _rotation.conj();
			osg::Vec3d eye = _center + _toWorld(inverseRotation * osg::Vec3d(0.0, 0.0, 1.0));
			osg::Vec3d fwd = _toWorld(inverseRotation * osg::Vec3d(0.0, 0.0, -1.0));

			// Signed depth of the scene center along the view axis.
			double depth = (osg::Vec3d(bs.center()) - eye) * fwd;
			double r = bs.radius() * 1.1; // 10% padding

			nearPlane = depth - r;
			farPlane = depth + r;
		}
	}

	cam.setProjectionMatrixAsOrtho(
		-h * aspect, h * aspect,
		-h, h,
		nearPlane, farPlane
	);
}

void Ortho2DManipulator::home(const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter& aa) {
	_rotation = osg::Quat();
	_yawAngle = 0.0;
	_pitchAngle = 0.0;

	if(_node.valid()) {
		auto bs = _node->getBound();

		_center = osg::Vec3d(bs.center());
		_halfExtentY = (bs.radius() > 0.0) ? bs.radius() * 1.2 : 1.0;
	}

	else {
		_center.set(0.0, 0.0, 0.0);
		_halfExtentY = 1.0;
	}

	aa.requestRedraw();
}

bool Ortho2DManipulator::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) {
	switch(ea.getEventType()) {
	case osgGA::GUIEventAdapter::PUSH:
		_lastPointer.set(ea.getXnormalized(), ea.getYnormalized());
		_dragging = (ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON);

		return false;

	case osgGA::GUIEventAdapter::RELEASE:
		_dragging = false;

		return false;

	case osgGA::GUIEventAdapter::DRAG: {
		if(!_dragging) return false;

		double nx = ea.getXnormalized();
		double ny = ea.getYnormalized();
		double dx = nx - _lastPointer.x();
		double dy = ny - _lastPointer.y();

		_lastPointer.set(nx, ny);

		bool ctrl = (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_CTRL) != 0;

		if(ctrl) {
			// 3D: pitch/yaw orbit around center. Tracked as two independent scalar angles and
			// reconstructed fresh each time (NOT accumulated onto the previous _rotation via a
			// path-dependent "current right" axis) - yaw always rotates around fixed screen up,
			// pitch always around the fixed original screen right, so yaw behaves identically
			// regardless of how much pitch has already accumulated. Mixing a path-dependent axis
			// (the old "current right", derived from _rotation itself) with a fixed one is
			// exactly what made yaw appear to rotate around the wrong axis after any pitch.
			// Clamped just short of +-90 degrees to avoid the one unavoidable Euler-angle
			// singularity (true gimbal lock, where yaw and roll become indistinguishable).
			//
			// _invertY/_invertX apply ONLY here (Ctrl-drag pitch/yaw) - plain pan below is
			// unaffected. Yaw's default (un-inverted) sign matches a direct-manipulation "grab and
			// drag" feel: dragging right rotates the model's near side to the right.
			static const double MAX_PITCH = osg::DegreesToRadians(89.0);

			double pitchDy = _invertY ? -dy : dy;
			double yawDx = _invertX ? -dx : dx;

			_yawAngle += yawDx * _rotateSensitivity;
			_pitchAngle = std::clamp(_pitchAngle + pitchDy * _rotateSensitivity, -MAX_PITCH, MAX_PITCH);

			// NOTE: this order looks backwards vs. standard quaternion composition (where
			// q1*q2 applied via .rotate() applies q2 first), but OSG's row-vector convention
			// reverses that for a product quaternion converted via Matrixd::rotate() + v*M:
			// verified empirically that this order applies pitch first (around the fixed
			// original screen right), then yaw (around fixed screen up) - the order needed so yaw
			// never drifts the apparent pitch. Swapping this order was the actual fix for a
			// real reported bug where any yaw after a pitch rotated around the wrong axis.
			_rotation =
				osg::Quat(_pitchAngle, osg::Vec3d(1.0, 0.0, 0.0)) *
				osg::Quat(_yawAngle, osg::Vec3d(0.0, 1.0, 0.0))
			;
		}

		else {
			// Pan along camera right/up so speed is constant regardless of rotation.
			double aspect = ea.getWindowWidth() > 0
				? double(ea.getWindowWidth()) / double(ea.getWindowHeight())
				: 1.0
			;

			osg::Quat inverseRotation = _rotation.conj();
			osg::Vec3d right = _toWorld(inverseRotation * osg::Vec3d(1.0, 0.0, 0.0));
			osg::Vec3d up = _toWorld(inverseRotation * osg::Vec3d(0.0, 1.0, 0.0));

			_center -= right * dx * _halfExtentY * aspect;
			_center -= up * dy * _halfExtentY;
		}

		aa.requestRedraw();

		return false;
	}

	case osgGA::GUIEventAdapter::SCROLL: {
		bool shift = (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_SHIFT) != 0;
		bool up = (ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_UP);

		if(shift) {
			// Pixel-nudge: each click moves the visible boundary by exactly _pixelNudge pixels.
			int winH = ea.getWindowHeight();
			double worldPerPixel = 2.0 * _halfExtentY / double(winH > 0 ? winH : 1);

			_halfExtentY += (up ? -1.0 : 1.0) * _pixelNudge * worldPerPixel;
		}

		else _halfExtentY *= up ? (1.0 / _wheelZoomFactor) : _wheelZoomFactor;

		_halfExtentY = std::clamp(_halfExtentY, _halfExtentLimits.x(), _halfExtentLimits.y());

		aa.requestRedraw();

		return true;
	}

	case osgGA::GUIEventAdapter::KEYDOWN:
		if(
			ea.getKey() == osgGA::GUIEventAdapter::KEY_Space ||
			ea.getKey() == osgGA::GUIEventAdapter::KEY_Home
		) {
			home(ea, aa);
			return true;
		}

		return false;

	default:
		return false;
	}
}

void OrbitAxisManipulator::setUpAxis(const osg::Vec3d& up) {
	osg::Vec3d upAxis = up;

	if(!normalize(upAxis)) return;

	osg::Vec3d homeDirection = _homeDirection - upAxis * (_homeDirection * upAxis);

	if(!normalize(homeDirection)) homeDirection = perpendicularTo(upAxis);

	_upAxis = upAxis;
	_homeDirection = homeDirection;
}

void OrbitAxisManipulator::setHomeDirection(const osg::Vec3d& direction) {
	osg::Vec3d homeDirection = direction - _upAxis * (direction * _upAxis);

	if(!normalize(homeDirection)) return;

	_homeDirection = homeDirection;
}

// Eye position, cylindrical around the guide line. Look-at target is the guide line at the SAME
// axial position as the eye, so the view direction always stays horizontal (never pitches).
osg::Matrixd OrbitAxisManipulator::getInverseMatrix() const {
	double s = std::sin(_yaw);
	double c = std::cos(_yaw);
	osg::Vec3d center = _axis + _upAxis * _height;
	osg::Vec3d radial = _homeDirection * c + _orbitRight() * s;
	osg::Vec3d eye = center + radial * _distance;

	return osg::Matrixd::lookAt(eye, center, _upAxis);
}

osg::Matrixd OrbitAxisManipulator::getMatrix() const {
	return osg::Matrixd::inverse(getInverseMatrix());
}

// Camera-to-world: derive yaw/axial height/distance from the eye position, keeping the guide line
// (_axis/_axialLimits) as already established by setNode()/home().
void OrbitAxisManipulator::setByMatrix(const osg::Matrixd& m) {
	osg::Vec3d eye = m.getTrans();
	osg::Vec3d offset = eye - _axis;
	double height = offset * _upAxis;
	osg::Vec3d radial = offset - _upAxis * height;

	_distance = radial.length();
	_yaw = std::atan2(radial * _orbitRight(), radial * _homeDirection);
	_height = height;

	if(_hasHeightReference) _clampHeight();
}

void OrbitAxisManipulator::setByInverseMatrix(const osg::Matrixd& m) {
	setByMatrix(osg::Matrixd::inverse(m));
}

// Recompute the distance clamp from the camera's current vertical FOV and the model's vertical
// extent, then clamp _distance to it. Leaves the projection matrix untouched - the caller owns
// FOV/near/far.
void OrbitAxisManipulator::updateCamera(osg::Camera& cam) {
	double fovy, aspect, zNear, zFar;

	if(cam.getProjectionMatrix().getPerspective(fovy, aspect, zNear, zFar)) {
		double modelHeight = _axialLimits.y() - _axialLimits.x();
		double tanHalfFovy = std::tan(osg::DegreesToRadians(fovy) * 0.5);

		if(modelHeight > 0.0 && tanHalfFovy > 0.0) {
			// coverage = modelHeight / (2 * distance * tanHalfFovy) => distance = modelHeight / (2 * coverage * tanHalfFovy)
			_distanceLimits.set(
				modelHeight / (2.0 * _coverageLimits.y() * tanHalfFovy),
				modelHeight / (2.0 * _coverageLimits.x() * tanHalfFovy)
			);
		}
	}

	_distance = std::clamp(_distance, _distanceLimits.x(), _distanceLimits.y());

	cam.setViewMatrix(getInverseMatrix());
}

void OrbitAxisManipulator::home(const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter& aa) {
	_yaw = 0.0;

	if(_node.valid()) {
		auto bs = _node->getBound();
		osg::Vec3d center(bs.center());
		double centerHeight = center * _upAxis;

		_axis = center - _upAxis * centerHeight;
		_axialLimits.set(centerHeight - bs.radius(), centerHeight + bs.radius());
		_height = centerHeight;
		_distance = (bs.radius() > 0.0) ? bs.radius() * 3.0 : 3.0;
	}

	else {
		_axis.set(0.0, 0.0, 0.0);
		_axialLimits.set(-1.0, 1.0);
		_height = 0.0;
		_distance = 3.0;
	}

	_hasHeightReference = true;
	_clampHeight();

	aa.requestRedraw();
}

void OrbitAxisManipulator::orbitByDelta(double dx, double dy) {
	if(_invertX) dx = -dx;
	if(_invertY) dy = -dy;

	_yaw += dx * _yawSensitivity;

	if(_hasHeightReference) {
		const auto& limits = _effectiveHeightLimits();

		_height += dy * _heightSensitivity * (limits.y() - limits.x());
		_clampHeight();
	}
}

// Always active: MOVE and DRAG are handled identically, with no button gate. The first event
// after construction (or after home()) just seeds _lastPointer so we don't apply a spurious
// jump on the initial mouse position.
void OrbitAxisManipulator::_orbit(double nx, double ny) {
	if(!_initialized) {
		_lastPointer.set(nx, ny);
		_initialized = true;

		return;
	}

	double dx = nx - _lastPointer.x();
	double dy = ny - _lastPointer.y();

	_lastPointer.set(nx, ny);

	orbitByDelta(dx, dy);
}

bool OrbitAxisManipulator::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) {
	switch(ea.getEventType()) {
	case osgGA::GUIEventAdapter::MOVE:
	case osgGA::GUIEventAdapter::DRAG:
		if(_liveOrbitEnabled) {
			_orbit(ea.getXnormalized(), ea.getYnormalized());
			aa.requestRedraw();
		}

		return false;

	case osgGA::GUIEventAdapter::SCROLL: {
		bool up = (ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_UP);

		_distance *= up ? (1.0 / _wheelZoomFactor) : _wheelZoomFactor;
		_distance = std::clamp(_distance, _distanceLimits.x(), _distanceLimits.y());

		aa.requestRedraw();

		return true;
	}

	case osgGA::GUIEventAdapter::KEYDOWN:
		if(
			ea.getKey() == osgGA::GUIEventAdapter::KEY_Space ||
			ea.getKey() == osgGA::GUIEventAdapter::KEY_Home
		) {
			home(ea, aa);
			return true;
		}

		return false;

	default:
		return false;
	}
}

}
