#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Callback>
#include <osg/Camera>
#include <osg/CullSettings>
#include <osg/Math>
#include <osg/Matrix>
#include <osg/Vec2>
#include <osg/observer_ptr>
#include <osgGA/CameraManipulator>
#include <osgGA/GUIActionAdapter>
#include <osgGA/GUIEventAdapter>
#include <osgGA/TrackballManipulator>

OSGX_ENABLE_WARNINGS

#include <cmath>
#include <concepts>
#include <utility>
#include <vector>

namespace osgx {

// ================================================================================================
// MultiCameraManipulator
//
// Composite camera manipulator that routes input to one active manipulator while letting targets
// drive either the viewer's main camera or a dedicated camera such as an RTT camera.
// ================================================================================================
class MultiCameraManipulator: public osgGA::CameraManipulator {
public:
	struct Target {
		std::string name;
		osg::ref_ptr<osgGA::CameraManipulator> manipulator;
		osg::observer_ptr<osg::Camera> camera;
		osg::observer_ptr<osg::Node> scene;
		std::function<void(bool)> setActive;
	};

	void setToggleKey(int key) { _toggleKey = key; }
	int getToggleKey() const { return _toggleKey; }

	void addTarget(
		const std::string& name,
		osgGA::CameraManipulator* manipulator,
		osg::Camera* camera=nullptr,
		osg::Node* scene=nullptr,
		std::function<void(bool)> setActive={}
	);

	unsigned int getActiveIndex() const { return _active; }
	unsigned int getNumTargets() const { return static_cast<unsigned int>(_targets.size()); }

	Target* activeTarget() {
		return _active < _targets.size() ? &_targets[_active] : nullptr;
	}

	const Target* activeTarget() const {
		return _active < _targets.size() ? &_targets[_active] : nullptr;
	}

	void activate(unsigned int index);

	void next() {
		if(!_targets.empty()) activate((_active + 1u) % static_cast<unsigned int>(_targets.size()));
	}

	void setByMatrix(const osg::Matrixd& matrix) override;
	void setByInverseMatrix(const osg::Matrixd& matrix) override;
	osg::Matrixd getMatrix() const override;
	osg::Matrixd getInverseMatrix() const override;
	void updateCamera(osg::Camera& mainCamera) override;
	void setNode(osg::Node* node) override;
	osg::Node* getNode() override;
	const osg::Node* getNode() const override;
	void home(double currentTime) override;
	void home(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;
	void init(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;
	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;

private:
	void _updateCamera(osg::Camera& mainCamera);

	std::vector<Target> _targets;
	osg::observer_ptr<osg::Node> _defaultScene;
	unsigned int _active = 0;
	int _toggleKey = 'x';
	bool _hasActive = false;
	bool _mainCameraUpdated = false;
};

// ================================================================================================
// Ortho2DManipulator
//
// Pan/zoom camera manipulator for orthographic 2D scenes.
//
// Controls:
//
// Left drag pan in the configured 2D plane
// Scroll geometric zoom (_wheelZoomFactor per click)
// Shift+Scroll pixel-nudge zoom (_pixelNudge screen pixels per click)
// Ctrl+Left drag 3D pitch/yaw (unlocks rotation around center)
// Space / Home reset to home
//
// The manipulator owns the projection matrix: updateCamera() sets both view and
// projection each frame, so callers do NOT need to configure the camera projection
// separately.
//
// TODO: consider delegating to or toggling a full OrbitManipulator for persistent
// 3D navigation (currently Ctrl+drag accumulates rotation, release keeps it).
// ================================================================================================
class Ortho2DManipulator: public osgGA::CameraManipulator {
public:
	OSGX_META_Object(osgx, Ortho2DManipulator)

	Ortho2DManipulator() = default;

	OSGX_DISABLE_WARNINGS

		Ortho2DManipulator(
			const Ortho2DManipulator& m,
			const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY
		):
		osgGA::CameraManipulator(m, co),
		_center(m._center),
		_halfExtentY(m._halfExtentY),
		_halfExtentLimits(m._halfExtentLimits),
		_pixelNudge(m._pixelNudge),
		_wheelZoomFactor(m._wheelZoomFactor),
		_rotateSensitivity(m._rotateSensitivity),
		_invertY(m._invertY),
		_invertX(m._invertX),
		_planeNormal(m._planeNormal),
		_screenUp(m._screenUp),
		_rotation(m._rotation),
		_yawAngle(m._yawAngle),
		_pitchAngle(m._pitchAngle),
		_node(m._node) {}

	OSGX_ENABLE_WARNINGS

	// Config
	void setPixelNudge(double n) { _pixelNudge = n; }
	double getPixelNudge() const { return _pixelNudge; }

	void setWheelZoomFactor(double f) { _wheelZoomFactor = f; }
	double getWheelZoomFactor() const { return _wheelZoomFactor; }

	void setZoomLimits(double minH, double maxH) { _halfExtentLimits.set(minH, maxH); }
	std::pair<double, double> getZoomLimits() const { return {_halfExtentLimits.x(), _halfExtentLimits.y()}; }

	void setRotateSensitivity(double s) { _rotateSensitivity = s; }
	double getRotateSensitivity() const { return _rotateSensitivity; }

	// Inverts the Y axis used by the Ctrl+drag 3D pitch/yaw tilt ONLY - plain (non-Ctrl) pan is
	// unaffected. Default true: dragging up tilts the view up. Set false to restore the raw,
	// uninverted feel.
	void setInvertY(bool invert) { _invertY = invert; }
	bool getInvertY() const { return _invertY; }

	// Inverts the X axis used by the Ctrl+drag 3D yaw ONLY - plain (non-Ctrl) pan is unaffected.
	// Default false: dragging right rotates the model's near side to the right (matching a
	// direct-manipulation "grab and drag" feel). Set true to restore the raw, uninverted feel.
	void setInvertX(bool invert) { _invertX = invert; }
	bool getInvertX() const { return _invertX; }

	// The unrotated 2D plane. Defaults to XY with a +Z normal and +Y at screen top.
	// screenUp is orthogonalized against planeNormal; a zero normal is ignored and a parallel
	// existing screenUp is replaced with a stable perpendicular direction.
	void setPlaneNormal(const osg::Vec3d& normal);
	const osg::Vec3d& getPlaneNormal() const { return _planeNormal; }
	void setScreenUp(const osg::Vec3d& up);
	const osg::Vec3d& getScreenUp() const { return _screenUp; }

	// State
	void setCenter(const osg::Vec3d& c) { _center = c; }
	const osg::Vec3d& getCenter() const { return _center; }

	void setHalfExtentY(double h) {
		_halfExtentY = std::clamp(h, _halfExtentLimits.x(), _halfExtentLimits.y());
	}

	double getHalfExtentY() const { return _halfExtentY; }

	// CameraManipulator interface
	void setNode(osg::Node* node) override { _node = node; }
	const osg::Node* getNode() const override { return _node.get(); }
	osg::Node* getNode() override { return _node.get(); }

	// Extract pan center from the translation component of the camera-to-world matrix.
	void setByMatrix(const osg::Matrixd& m) override;
	void setByInverseMatrix(const osg::Matrixd& m) override;

	// Camera-to-world: undo the view matrix composition.
	osg::Matrixd getMatrix() const override;

	// World-to-camera (view matrix).
	// Orbit convention: center to origin -> rotate (pivot is now at origin) -> pull back.
	// OSG uses row vectors, so A*B*C applies A first; pull-back must come last.
	osg::Matrixd getInverseMatrix() const override;

	// Sets BOTH view and projection so the caller owns neither.
	//
	// We take ownership of near/far (DO_NOT_COMPUTE_NEAR_FAR) because OSG's bounding-volume
	// computation clamps near > 0 even for ortho, which clips geometry that lands at negative
	// depth when the camera is tilted in 3D. We derive tight near/far analytically from the
	// scene bounding sphere each frame instead.
	void updateCamera(osg::Camera& cam) override;

	void home(const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter& aa) override;
	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;

private:
	osg::Vec3d _right() const { return _screenUp ^ _planeNormal; }
	osg::Vec3d _toWorld(const osg::Vec3d& local) const {
		return _right() * local.x() + _screenUp * local.y() + _planeNormal * local.z();
	}

	osg::Vec3d _center{0.0, 0.0, 0.0};

	double _halfExtentY{1.0};
	osg::Vec2d _halfExtentLimits{1e-4, 1e6};
	double _pixelNudge{1.0};
	double _wheelZoomFactor{1.15};
	double _rotateSensitivity{2.0};
	bool _invertY{true};
	bool _invertX{false};
	osg::Vec3d _planeNormal{0.0, 0.0, 1.0};
	osg::Vec3d _screenUp{0.0, 1.0, 0.0};

	osg::Quat _rotation; // identity = pure top-down 2D
	double _yawAngle{0.0}; // screenUp; independent of _pitchAngle - see handle()'s ctrl branch
	double _pitchAngle{0.0}; // screen right, clamped short of +-90 degrees
	osg::ref_ptr<osg::Node> _node;

	bool _dragging{false};
	osg::Vec2d _lastPointer{0.0, 0.0};
};

// ================================================================================================
// OrbitAxisManipulator
//
// "Turntable" camera manipulator for model-viewer-style presentation (see: the Batman Arkham
// series' character/suit viewer). The camera orbits a fixed up-axis guide line through the
// model's bounds, always looking level (never pitching up/down) at whatever height it's currently
// at, and dollies toward/away from that line on zoom. The model itself never moves or scales.
//
// Controls:
//
// Mouse move/drag (no button required) orbit (X) + height (Y), always active
// Scroll dolly zoom, clamped by viewport-coverage fraction (see below)
// Space / Home reset to home
//
// State is cylindrical: yaw around the guide line, axial height along it (clamped to the bound's
// extent along the configured up axis), and distance from it (clamped
// so the model can't be zoomed past ~50% visible or zoomed out past a ~5% viewport margin, in
// terms of the camera's current vertical FOV - see updateCamera()).
//
// The manipulator does NOT own the projection matrix (unlike Ortho2DManipulator) - it reads the
// camera's existing perspective FOV each frame to recompute the distance clamp, but leaves
// near/far/FOV to the caller.
//
// v1 tracks raw mouse position deltas (bounded by the window edges, like a trackpad) rather than
// true relative/captured motion - no cursor hide or pointer warp/confine. That's real OS-specific
// plumbing (X11 XGrabPointer/XWarpPointer and friends); see TODO.md for the planned move of the
// existing pyosg/linux platform helpers into osgx before adding it here.
//
// TODO: add optional "gate action X behind button Y" modes (e.g. require LEFT_MOUSE_BUTTON held
// for orbit/height) once the always-active feel is validated - deliberately left out for now.
// ================================================================================================
class OrbitAxisManipulator: public osgGA::CameraManipulator {
public:
	OSGX_META_Object(osgx, OrbitAxisManipulator)

	OrbitAxisManipulator() = default;

	OSGX_DISABLE_WARNINGS

		OrbitAxisManipulator(
			const OrbitAxisManipulator& m,
			const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY
		):
		osgGA::CameraManipulator(m, co),
		_axis(m._axis),
		_axialLimits(m._axialLimits),
		_heightLimits(m._heightLimits),
		_hasHeightLimits(m._hasHeightLimits),
		_hasHeightReference(m._hasHeightReference),
		_height(m._height),
		_yaw(m._yaw),
		_distance(m._distance),
		_distanceLimits(m._distanceLimits),
		_coverageLimits(m._coverageLimits),
		_yawSensitivity(m._yawSensitivity),
		_heightSensitivity(m._heightSensitivity),
		_wheelZoomFactor(m._wheelZoomFactor),
		_invertX(m._invertX),
		_invertY(m._invertY),
		_upAxis(m._upAxis),
		_homeDirection(m._homeDirection),
		_node(m._node) {}

	OSGX_ENABLE_WARNINGS

	// Config
	void setYawSensitivity(double s) { _yawSensitivity = s; }
	void setHeightSensitivity(double s) { _heightSensitivity = s; }
	void setWheelZoomFactor(double f) { _wheelZoomFactor = f; }

	// minCoverage/maxCoverage are fractions of the viewport's up-axis extent that the model's
	// bound should occupy at the zoomed-out/zoomed-in extremes, respectively (e.g. 0.95 = 5%
	// margin top/bottom when zoomed out; 2.0 = model is 2x viewport height, ~50% visible, when
	// zoomed in).
	void setCoverageLimits(double minCoverage, double maxCoverage) {
		_coverageLimits.set(minCoverage, maxCoverage);
	}

	// Restricts the camera's axial height to the inclusive [minHeight, maxHeight] interval.
	// Heights are signed world-space distances along upAxis, so for the default Z-up frame they
	// are simply world Z coordinates. This does not affect model framing or dolly limits.
	//
	// Calling this before home() is safe: the range is remembered and applied only once home()
	// establishes the subject's reference axis and height.
	void setHeightLimits(double minHeight, double maxHeight) {
		if(minHeight > maxHeight) std::swap(minHeight, maxHeight);

		_heightLimits.set(minHeight, maxHeight);
		_hasHeightLimits = true;

		if(_hasHeightReference) _clampHeight();
	}

	// Restores the automatic range derived from the current subject bounds.
	void clearHeightLimits() {
		_hasHeightLimits = false;

		if(_hasHeightReference) _clampHeight();
	}

	bool hasHeightLimits() const { return _hasHeightLimits; }
	std::pair<double, double> getHeightLimits() const {
		const auto& limits = _effectiveHeightLimits();

		return {limits.x(), limits.y()};
	}

	std::pair<double, double> getCoverageLimits() const { return {_coverageLimits.x(), _coverageLimits.y()}; }
	double getYawSensitivity() const { return _yawSensitivity; }
	double getHeightSensitivity() const { return _heightSensitivity; }
	double getWheelZoomFactor() const { return _wheelZoomFactor; }

	// Inverts the pointer X axis used for yaw (both the raw MOVE/DRAG path and orbitByDelta()).
	void setInvertX(bool invert) { _invertX = invert; }
	bool getInvertX() const { return _invertX; }

	// Inverts the pointer Y axis used for axial motion (both the raw MOVE/DRAG path and
	// orbitByDelta()).
	// Default true: dragging/moving up raises the camera. Set false to restore the raw,
	// uninverted feel.
	void setInvertY(bool invert) { _invertY = invert; }
	bool getInvertY() const { return _invertY; }

	// The turntable frame. Defaults to Z-up, looking from -Y at yaw == 0.
	// homeDirection is projected onto the plane perpendicular to upAxis. A zero up axis is
	// ignored; changing upAxis with a parallel existing homeDirection chooses a stable fallback.
	void setUpAxis(const osg::Vec3d& up);
	const osg::Vec3d& getUpAxis() const { return _upAxis; }
	void setHomeDirection(const osg::Vec3d& direction);
	const osg::Vec3d& getHomeDirection() const { return _homeDirection; }

	// State
	double getYaw() const { return _yaw; }
	double getHeight() const { return _height; }
	double getDistance() const { return _distance; }

	// Applies a pre-computed (dx, dy) directly, in the same normalized (roughly [-1, 1] per axis)
	// units as GUIEventAdapter::getXnormalized()/getYnormalized() - the same units handle()
	// itself derives internally via _orbit(). This is the hook for driving orbit/height from
	// something other than raw MOVE/DRAG events, e.g. osgx::CursorCapture's accumulated delta
	// (normalize its pixel delta by the window's half-width/half-height first to match this
	// scale). Deliberately NOT wired to CursorCapture internally - see the layering note on
	// osgx::CursorCapture in osgx/Cursor.hpp.
	void orbitByDelta(double dx, double dy);

	// Disables the raw MOVE/DRAG-driven orbit path (handle()'s call into _orbit()) without
	// affecting scroll-zoom or Space/Home reset. orbitByDelta() always works regardless of this
	// flag. Exists so an external cursor-capture scheme (e.g. osgx::CursorCapture) can
	// drive orbitByDelta() exclusively: osgViewer::Viewer::eventTraversal() delivers every event
	// to the camera manipulator AND every other installed GUIEventHandler unconditionally (a
	// handler's return value does not stop propagation to the others), so without this the
	// manipulator would independently re-track the same raw cursor position, AND misinterpret a
	// capture scheme's own warp-to-center jumps as huge manual drags. Re-enabling reseeds the
	// next MOVE/DRAG event so there's no spurious jump from wherever the cursor drifted while
	// disabled.
	void setLiveOrbitEnabled(bool enabled) {
		_liveOrbitEnabled = enabled;

		if(enabled) _initialized = false;
	}

	bool isLiveOrbitEnabled() const { return _liveOrbitEnabled; }

	// CameraManipulator interface
	void setNode(osg::Node* node) override {
		_node = node;
		_hasHeightReference = false;
	}
	const osg::Node* getNode() const override { return _node.get(); }
	osg::Node* getNode() override { return _node.get(); }

	void setByMatrix(const osg::Matrixd& m) override;
	void setByInverseMatrix(const osg::Matrixd& m) override;
	osg::Matrixd getMatrix() const override;
	osg::Matrixd getInverseMatrix() const override;

	// Leaves the projection matrix untouched; only recomputes the distance clamp (from the
	// camera's current vertical FOV and the model's bound) and sets the view matrix.
	void updateCamera(osg::Camera& cam) override;

	void home(const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter& aa) override;
	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;

private:
	void _orbit(double nx, double ny);
	const osg::Vec2d& _effectiveHeightLimits() const {
		return _hasHeightLimits ? _heightLimits : _axialLimits;
	}

	void _clampHeight() {
		const auto& limits = _effectiveHeightLimits();

		_height = std::clamp(_height, limits.x(), limits.y());
	}

	osg::Vec3d _orbitRight() const { return _upAxis ^ _homeDirection; }

	osg::Vec3d _axis{0.0, 0.0, 0.0}; // point on the guide line
	osg::Vec2d _axialLimits{-0.5, 0.5};
	osg::Vec2d _heightLimits{-0.5, 0.5};
	bool _hasHeightLimits{false};
	bool _hasHeightReference{false};
	double _height{0.0}; // signed distance along _upAxis from _axis
	double _yaw{0.0};
	double _distance{1.0};
	osg::Vec2d _distanceLimits{1e-4, 1e6};
	osg::Vec2d _coverageLimits{0.95, 2.0};
	double _yawSensitivity{osg::PI};
	double _heightSensitivity{0.5};
	double _wheelZoomFactor{1.15};
	bool _invertX{true};
	bool _invertY{true};
	osg::Vec3d _upAxis{0.0, 0.0, 1.0};
	osg::Vec3d _homeDirection{0.0, -1.0, 0.0};

	osg::ref_ptr<osg::Node> _node;

	bool _initialized{false};
	bool _liveOrbitEnabled{true};
	osg::Vec2d _lastPointer{0.0, 0.0};
};

// ================================================================================================
// CameraManipulator<Base>
//
// CRTP mixin (same idiom as osgx::Array<T>, see osgx/Array.hpp) that lets a manipulator merge
// one-shot or persistent "camera intents" - a fly-to animation, a shake, agent-driven nudges --
// onto itself, without a caller needing a second manipulator object or to know/care which concrete
// manipulator type is in play. osgx::CameraManipulator<osgGA::TrackballManipulator> genuinely IS a
// TrackballManipulator: every interaction method (handle, home, getMatrix, setNode, ...) is
// inherited directly, not forwarded through a held ref_ptr.
//
// Intents are plain osg::Callback subclasses (see osgx/CameraIntents.hpp for FlyToCallback/
// ShakeCallback), added via addUpdateCameraCallback(). This deliberately reuses OSG's own callback
// type rather than a bespoke hierarchy, so a caller can drop in either a purpose-built C++
// subclass or (once pyx::CallableCallback grows a matching specialization) a plain Python callable.
//
// updateCamera() always runs Base::updateCamera(camera) first to establish this frame's normal
// baseline pose, then runs each attached callback in attachment order, letting each one further
// mutate camera.viewMatrix (a ShakeCallback composes on top of whatever's already there; a
// FlyToCallback unconditionally overwrites it with an interpolated pose while active). This is
// NOT SUPPORTED when Base is osgx::MultiCameraManipulator - MultiCameraManipulator::updateCamera()
// can route its real output to a DIFFERENT osg::Camera than the one passed in (see its own
// per-target camera), which would silently desync from this mixin's callback loop.
//
// NOTE: no OSGX_META_Object / copy constructor (matches MultiCameraManipulator, not
// Ortho2DManipulator/OrbitAxisManipulator) - clone()/copy-construction will NOT propagate the
// attached callback list. Manipulators are rarely cloned; not solved here.
// ================================================================================================
template<typename T>
concept OSGCameraManipulator = std::derived_from<T, osgGA::CameraManipulator>;

// Type-erases CameraManipulator<Base>'s extra surface so a generic osg::Callback - which only
// ever receives a plain osg::Object* - can reach back into "whatever manipulator it's attached
// to" without needing to know Base. dynamic_cast across this is safe regardless of Base: OSG never
// disables RTTI, and osg::Object has a virtual destructor, so it stays live throughout.
class CameraIntentHost {
public:
	virtual double currentTime() const = 0;
	virtual void addUpdateCameraCallback(osg::Callback* cb, bool runOnce=false) = 0;
	virtual void removeUpdateCameraCallback(osg::Callback* cb) = 0;

protected:
	virtual ~CameraIntentHost() {}
};

template<OSGCameraManipulator Base=osgGA::TrackballManipulator>
class CameraManipulator: public Base, public CameraIntentHost {
public:
	using Base::Base;

	double currentTime() const override { return _currentTime; }

	void addUpdateCameraCallback(osg::Callback* cb, bool runOnce=false) override {
		_pendingAdds.push_back({cb, runOnce});
	}

	void removeUpdateCameraCallback(osg::Callback* cb) override {
		_pendingRemoves.push_back(cb);
	}

	// Read-only introspection of what's currently attached - reflects _callbacks as of the last
	// completed updateCamera() call: an addUpdateCameraCallback()/removeUpdateCameraCallback() made
	// from outside a frame (e.g. from a Python REPL between frames) only lands in _pendingAdds/
	// _pendingRemoves until the NEXT updateCamera(), so these can lag by up to one frame behind a
	// call that was just made - not a bug, the same async-apply design that protects the callback
	// loop in updateCamera() from mutating _callbacks mid-iteration.
	unsigned int getNumUpdateCameraCallbacks() const {
		return static_cast<unsigned int>(_callbacks.size());
	}

	osg::Callback* getUpdateCameraCallback(unsigned int i) const {
		return _callbacks[i].callback.get();
	}

	bool getUpdateCameraCallbackRunOnce(unsigned int i) const {
		return _callbacks[i].runOnce;
	}

	// Peeks at FRAME events to cache the current time for intents to read via currentTime() --
	// matches how OSG's own animated manipulators source time (from the FRAME event, not a polled
	// osg::Timer), and keeps intent timing deterministically testable later by injecting FRAME
	// events. Always forwards to Base - this is a peek, never an interception.
	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override {
		if(ea.getEventType() == osgGA::GUIEventAdapter::FRAME) _currentTime = ea.getTime();

		return Base::handle(ea, aa);
	}

	void updateCamera(osg::Camera& camera) override {
		Base::updateCamera(camera);

		_applyPending();

		// Iterate by index, not range-for/iterators: a callback's own run() may call
		// addUpdateCameraCallback()/removeUpdateCameraCallback() on `this` (e.g. a finishing
		// FlyToCallback chaining into a persistent effect), which must not mutate _callbacks while
		// it's being iterated - those calls only stage into _pendingAdds/_pendingRemoves, applied
		// after this loop finishes.
		for(size_t i = 0; i < _callbacks.size(); i++) {
			auto& entry = _callbacks[i];

			// Local convention for this call site only (not OSG's generic traverse-continuation
			// meaning): true = still active, keep in the list; false = done. A false return only
			// causes removal if runOnce is true - a persistent entry (runOnce=false) stays
			// regardless of what it returns. So runOnce means "auto-remove when I signal done," not
			// literally "called exactly once" - a FlyToCallback legitimately runs across many
			// frames before finally returning false.
			bool active = entry.callback->run(this, &camera);

			if(!active && entry.runOnce) entry.callback = nullptr; // mark for sweep below
		}

		std::erase_if(_callbacks, [](const Entry& e) { return !e.callback.valid(); });

		_applyPending();
	}

private:
	struct Entry {
		osg::ref_ptr<osg::Callback> callback;
		bool runOnce;
	};

	void _applyPending() {
		if(!_pendingRemoves.empty()) {
			for(auto* cb : _pendingRemoves) {
				std::erase_if(_callbacks, [&](const Entry& e) { return e.callback == cb; });
			}

			_pendingRemoves.clear();
		}

		if(!_pendingAdds.empty()) {
			for(auto& entry : _pendingAdds) _callbacks.push_back(std::move(entry));

			_pendingAdds.clear();
		}
	}

	std::vector<Entry> _callbacks;
	std::vector<Entry> _pendingAdds;
	std::vector<osg::Callback*> _pendingRemoves;

	double _currentTime = 0.0;
};

}
