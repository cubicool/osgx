#pragma once

#include "Core.hpp"
#include "Cursor.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Callback>
#include <osg/Camera>
#include <osg/CullSettings>
#include <osg/Math>
#include <osg/Matrix>
#include <osg/Vec2>
#include <osg/observer_ptr>
#include <osgGA/CameraManipulator>
#include <osgGA/FirstPersonManipulator>
#include <osgGA/GUIActionAdapter>
#include <osgGA/GUIEventAdapter>
#include <osgGA/NodeTrackerManipulator>
#include <osgGA/StandardManipulator>
#include <osgGA/TrackballManipulator>
#include <osgViewer/View>

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
// PlayerManipulator<Base>
//
// Parameterized-base mixin (same idiom as osgx::ActionsManipulator<Base> below, and osgx::Array<T>
// in osgx/Array.hpp - NOT CRTP: Base here is the stock OSG class being extended, not the leaf that
// eventually inherits this template, so nothing ever reaches back down into a more-derived type).
// Shares the "modern game camera" look concerns - sensitivity, Y-invert, a configurable
// look-trigger button/style, AND osgx::CursorCapture hide+warp+accumulate - across every osgx
// gaming-style manipulator built on a different osgGA::StandardManipulator-derived Base
// (osgx::FirstPersonManipulator today; osgx::ThirdPersonManipulator below).
//
// Unlike osgx::OrbitAxisManipulator (see its own class comment), which deliberately leaves
// CursorCapture composed at the application level so it stays ignorant of any one capture scheme,
// this mixin owns a CursorCapture directly: it IS the opinionated "modern game camera" tier, where
// hide+warp+accumulate look is the expected default behavior, not an optional bolt-on.
//
// performMouseDeltaMovement(dx, dy) is the single hook both osgGA::FirstPersonManipulator and
// osgGA::OrbitManipulator (parent of NodeTrackerManipulator) independently override at the same
// StandardManipulator-declared signature - confirmed against the OSG 3.6.5 source, not assumed -
// so overriding it once here and forwarding to Base:: with an adjusted delta covers every look
// motion uniformly regardless of Base, whether it's driven by handle()'s FRAME-polled
// CursorCapture::drain() below (both LookStyle::ALWAYS and, while held, CLICK_HOLD) or by a
// leaf's own performMovement*Button override explicitly funneling into it as a fallback for when
// CursorCapture itself couldn't be constructed (see isCaptureActive()).
//
// A held-button look (LookStyle::CLICK_HOLD) still needs a leaf's OWN performMovement*Button
// override, because FirstPersonManipulator and OrbitManipulator map their LEFT/MIDDLE/RIGHT hooks
// to unrelated actions (FP has no native right-button behavior at all; Orbit uses left=rotate,
// middle=pan, right=zoom) - a uniform override here would wrongly apply look sensitivity to
// Orbit's pan/zoom. isLookButton() is what a leaf's override checks; see osgx::ThirdPersonManipulator
// for the shape. Deliberately NOT trying to preserve Base's own per-button math (e.g.
// OrbitManipulator's throw/momentum-aware rotateTrackball()) once CursorCapture takes over a held
// look-drag - every real motion sample while captured is a warp artifact as far as Base's own
// _ga_t0/_ga_t1-based dx/dy tracking is concerned (the same reason
// OrbitAxisManipulator::setLiveOrbitEnabled() exists), so a leaf's override must go fully inert
// for that button while isCaptureActive(), deferring entirely to the once-per-frame
// CursorCapture::drain() in handle() below instead of also processing the live drag event.
// ================================================================================================
template<typename T>
concept OSGStandardManipulator = std::derived_from<T, osgGA::StandardManipulator>;

// Hoisted out of PlayerManipulator<Base> itself - it doesn't involve Base at all, so keeping it a
// template-nested type would give every instantiation (PlayerManipulator<osgGA::FirstPersonManipulator>::LookStyle
// vs PlayerManipulator<osgGA::NodeTrackerManipulator>::LookStyle, ...) a DISTINCT C++ type despite
// identical meaning - harmless in C++ (nothing compares across them), but it would force
// osgx::FirstPersonManipulator and osgx::ThirdPersonManipulator's Python bindings into two
// separate py::enum_<> registrations with two separate Python types for what's conceptually one
// enum. PlayerManipulator<Base> re-exposes this as a member via `using LookStyle = osgx::LookStyle`
// below, so `SomeLeaf::LookStyle` qualified lookup (used by examples/osgx-manipulator.cpp) is
// unaffected.
enum class LookStyle {
	ALWAYS, // mouse look tracks every raw MOVE event, no button required
	CLICK_HOLD // mouse look only while getLookButton() is held and dragging
};

template<OSGStandardManipulator Base>
class PlayerManipulator: public Base {
public:
	using LookStyle = osgx::LookStyle;

	using Base::Base;

	OSGX_DISABLE_WARNINGS

		PlayerManipulator(
			const PlayerManipulator& m,
			const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY
		):
		Base(m, co),
		_lookStyle(m._lookStyle),
		_lookButton(m._lookButton),
		_sensitivity(m._sensitivity),
		_invertY(m._invertY) {}
		// _capture is deliberately NOT copied - it's tied to a specific osgViewer::View recovered
		// lazily from whichever GUIActionAdapter first reaches this instance's handle(), not
		// something a copy should inherit.

	OSGX_ENABLE_WARNINGS

	// Switching style immediately re-syncs capture to the new style's default (captured for
	// ALWAYS, released until the next look-button press for CLICK_HOLD) rather than leaving it in
	// whatever state the PREVIOUS style left it in.
	void setLookStyle(LookStyle style) {
		_lookStyle = style;

		if(_capture.valid()) _capture->setCaptured(style == LookStyle::ALWAYS);
	}

	LookStyle getLookStyle() const { return _lookStyle; }

	// Which mouse button LookStyle::CLICK_HOLD gates look (and CursorCapture) on; irrelevant under
	// LookStyle::ALWAYS. Default RIGHT_MOUSE_BUTTON. A leaf may still refuse to honor this for a
	// button it reserves for something else (osgx::FirstPersonManipulator unconditionally disables
	// LEFT, for example).
	void setLookButton(osgGA::GUIEventAdapter::MouseButtonMask button) { _lookButton = button; }
	osgGA::GUIEventAdapter::MouseButtonMask getLookButton() const { return _lookButton; }

	void setSensitivity(double s) { _sensitivity = s; }
	double getSensitivity() const { return _sensitivity; }

	// Default true: dragging/moving the mouse up looks up. Set false to restore the raw feel.
	void setInvertY(bool invert) { _invertY = invert; }
	bool getInvertY() const { return _invertY; }

protected:
	// True when `button` is the currently-configured look trigger under LookStyle::CLICK_HOLD -
	// see the class comment above for how a leaf uses this.
	bool isLookButton(osgGA::GUIEventAdapter::MouseButtonMask button) const {
		return _lookStyle == LookStyle::CLICK_HOLD && button == _lookButton;
	}

	// True once CursorCapture exists and is actively hiding/warping/accumulating - a leaf's own
	// performMovement*Button override checks this (see the class comment above) to go inert for
	// its look button while captured, instead of double-processing the same motion handle() below
	// already consumed this frame.
	bool isCaptureActive() const { return _capture.valid() && _capture->isCaptured(); }

	// The one place look sensitivity/invert-Y is computed.
	std::pair<double, double> adjustDelta(double dx, double dy) const {
		return {dx * _sensitivity, (_invertY ? -dy : dy) * _sensitivity};
	}

	bool performMouseDeltaMovement(const float dx, const float dy) override {
		auto [adjustedDx, adjustedDy] = adjustDelta(dx, dy);

		return Base::performMouseDeltaMovement(
			static_cast<float>(adjustedDx), static_cast<float>(adjustedDy)
		);
	}

	// Lazily recovers an osgViewer::View& the first time ANY event reaches this manipulator, to
	// construct the CursorCapture that osgx::CursorCapture's own constructor requires - a
	// CameraManipulator is never handed a View at construction time (the application calls
	// viewer.setCameraManipulator(this) well afterward), so there's no earlier point to build it.
	// The concrete GUIActionAdapter handed to handle() is normally the owning osgViewer::Viewer
	// itself, which IS-A View; retried (cheaply) on every call rather than giving up permanently if
	// an unusual host ever hands us something else.
	CursorCapture* _ensureCapture(osgGA::GUIActionAdapter& aa) {
		if(!_capture.valid()) {
			if(auto* view = dynamic_cast<osgViewer::View*>(&aa)) _capture = new CursorCapture(*view);
		}

		return _capture.get();
	}

	// Peeks at PUSH to start CursorCapture for LookStyle::CLICK_HOLD, keeps it permanently on for
	// LookStyle::ALWAYS, forwards every event into CursorCapture's own handle() (its hide+warp+
	// accumulate bookkeeping - a peek, never an interception, same convention as
	// ActionsManipulator<Base>::handle()), then applies this frame's accumulated delta through
	// performMouseDeltaMovement() once Base::handle() has run.
	//
	// RELEASE's capture-off is handled separately, AFTER Base::handle() runs rather than before -
	// see the comment down there for why that ordering specifically matters (it's load-bearing,
	// not cosmetic: it's what keeps OSG's own "throw" momentum from re-arming on release).
	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override {
		const auto type = ea.getEventType();
		const auto button = static_cast<osgGA::GUIEventAdapter::MouseButtonMask>(ea.getButton());

		if(type == osgGA::GUIEventAdapter::PUSH && isLookButton(button)) {
			if(auto* capture = _ensureCapture(us)) capture->setCaptured(true);
		}

		else if(type == osgGA::GUIEventAdapter::FRAME && _lookStyle == LookStyle::ALWAYS) {
			if(auto* capture = _ensureCapture(us); capture && !capture->isCaptured()) {
				capture->setCaptured(true);
			}
		}

		if(_capture.valid()) _capture->handle(ea, us);

		bool handled = Base::handle(ea, us);

		// StandardManipulator::handleMouseRelease() (called from inside Base::handle() above)
		// checks isMouseMoving() and, if true, performs ONE more performMovement*Button step
		// itself AND arms OSG's own "throw" momentum (_thrown) for every subsequent FRAME to keep
		// decaying - isMouseMoving() reads as true almost every time here, since CursorCapture has
		// been continuously warping the cursor all drag long, which StandardManipulator's own
		// _ga_t0/_ga_t1 history sees as constant motion regardless of the real net movement. Only
		// turning capture off AFTER that call - not before, like a naive PUSH/RELEASE-symmetric
		// implementation would - keeps isCaptureActive() reading true for THIS one call too, so a
		// leaf's performMovement*Button override (see its own class comment) stays inert for it
		// exactly like every other captured sample. That inertness is what actually prevents throw,
		// not a special case: performMovement() returning false means
		// `performMovement() && _allowThrow` is false regardless of _allowThrow, so _thrown never
		// gets armed in the first place - no separate setAllowThrow(false) needed.
		if(type == osgGA::GUIEventAdapter::RELEASE && isLookButton(button) && _capture.valid()) {
			_capture->setCaptured(false);
		}

		if(type == osgGA::GUIEventAdapter::FRAME && isCaptureActive()) {
			osg::Vec2 delta = _capture->drainNormalized(ea);

			if(delta.x() != 0.0f || delta.y() != 0.0f) {
				performMouseDeltaMovement(delta.x(), delta.y());

				us.requestRedraw();
			}
		}

		return handled;
	}

private:
	LookStyle _lookStyle = LookStyle::ALWAYS;
	osgGA::GUIEventAdapter::MouseButtonMask _lookButton = osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON;
	double _sensitivity = 1.0;
	bool _invertY = true;

	osg::ref_ptr<CursorCapture> _capture;
};

// ================================================================================================
// FirstPersonManipulator
//
// A osgx::PlayerManipulator<osgGA::FirstPersonManipulator> leaf adding the one thing the stock
// class is missing: continuous keyboard movement. The stock class is mouse-only - confirmed
// against the OSG source: neither it nor its StandardManipulator base bind any key for movement
// (StandardManipulator::handleKeyDown only binds Space -> home()). Look sensitivity, Y-invert, and
// LookStyle/look-button configuration all come from PlayerManipulator<Base> - this class owns only
// WASD.
//
// Controls:
//
// W/A/S/D move forward/back/strafe left/right, held continuously (scaled by frame delta time via
//          the inherited moveForward()/moveRight())
// Mouse    look, gated by getLookStyle()/getLookButton() (see PlayerManipulator<Base>); left-button
//          drag is unconditionally disabled here (reserved for a future "interact" binding instead
//          of look, regardless of what getLookButton() is set to)
// Scroll   step forward/back (inherited, unchanged)
// Space / Home reset to home (inherited, unchanged)
//
// LookStyle::ALWAYS is a real FPS mouse-look with no screen-edge limit - PlayerManipulator<Base>
// owns an osgx::CursorCapture directly (hide+warp+accumulate), unlike OrbitAxisManipulator's own
// raw-position tracking (see that class's own v1 caveat) which still composes CursorCapture at
// the application level instead.
// ================================================================================================
class FirstPersonManipulator: public PlayerManipulator<osgGA::FirstPersonManipulator> {
public:
	OSGX_META_Object(osgx, FirstPersonManipulator)

	explicit FirstPersonManipulator(int flags=DEFAULT_SETTINGS):
		PlayerManipulator<osgGA::FirstPersonManipulator>(flags) {}

	OSGX_DISABLE_WARNINGS

		FirstPersonManipulator(
			const FirstPersonManipulator& m,
			const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY
		):
		PlayerManipulator<osgGA::FirstPersonManipulator>(m, co),
		_moveSpeed(m._moveSpeed) {}

	OSGX_ENABLE_WARNINGS

	// Units per second, consumed by the inherited moveForward()/moveRight().
	void setMoveSpeed(double speed) { _moveSpeed = speed; }
	double getMoveSpeed() const { return _moveSpeed; }

protected:
	bool handleKeyDown(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override;
	bool handleKeyUp(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override;
	bool handleFrame(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override;
	bool performMovementLeftMouseButton(double eventTimeDelta, double dx, double dy) override;
	bool performMovementRightMouseButton(double eventTimeDelta, double dx, double dy) override;

private:
	enum MoveBit: unsigned int {
		MOVE_FORWARD = 0x01,
		MOVE_BACK = 0x02,
		MOVE_LEFT = 0x04,
		MOVE_RIGHT = 0x08
	};

	double _moveSpeed = 4.0;
	unsigned int _moveBits = 0;
};

// ================================================================================================
// ThirdPersonManipulator
//
// A osgx::PlayerManipulator<osgGA::NodeTrackerManipulator> leaf - a turntable-follow camera that
// orbits setTrackNode() (inherited from osgGA::NodeTrackerManipulator) at a configurable distance/
// height, always looking at it. Look sensitivity, Y-invert, and LookStyle/look-button
// configuration come from PlayerManipulator<Base>, exactly as osgx::FirstPersonManipulator; this
// class owns only node-tracking setup. No WASD - OrbitManipulator (NodeTrackerManipulator's base)
// has no equivalent to moveForward()/moveRight(), so there's no shared movement hook to add it
// through.
//
// osgGA::OrbitManipulator maps LEFT=rotate/MIDDLE=pan/RIGHT=zoom natively, and LEFT's rotate IS
// "look" here - so the default look button is LEFT, not PlayerManipulator<Base>'s RIGHT default,
// and LookStyle defaults to CLICK_HOLD, not ALWAYS (an orbit camera that spins on every raw mouse
// move with no button held would fight normal pan/zoom use). MIDDLE/RIGHT are left completely
// untouched (pan/zoom, not look) - only the LEFT-button override below is involved, and it funnels
// into performMouseDeltaMovement() exactly like FirstPersonManipulator's RIGHT-button override
// does, deliberately NOT preserving OrbitManipulator's own throw/momentum-aware rotate math (see
// PlayerManipulator<Base>'s own class comment for why: CursorCapture taking over a held look-drag
// makes Base's own per-event dx/dy tracking unusable anyway, so there is no throw-aware path left
// worth keeping once it does).
// ================================================================================================
class ThirdPersonManipulator: public PlayerManipulator<osgGA::NodeTrackerManipulator> {
public:
	OSGX_META_Object(osgx, ThirdPersonManipulator)

	ThirdPersonManipulator() {
		setTrackerMode(NODE_CENTER_AND_AZIM);
		setRotationMode(ELEVATION_AZIM);
		setLookStyle(LookStyle::CLICK_HOLD);
		setLookButton(osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON);
	}

	OSGX_DISABLE_WARNINGS

		ThirdPersonManipulator(
			const ThirdPersonManipulator& m,
			const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY
		):
		PlayerManipulator<osgGA::NodeTrackerManipulator>(m, co) {}

	OSGX_ENABLE_WARNINGS

protected:
	bool performMovementLeftMouseButton(double eventTimeDelta, double dx, double dy) override;
};

// ================================================================================================
// ActionsManipulator<Base>
//
// Parameterized-base mixin (same idiom as osgx::PlayerManipulator<Base> above, and osgx::Array<T>
// in osgx/Array.hpp - NOT CRTP: Base is the concrete manipulator type being extended, not the leaf
// inheriting this template, so nothing here ever reaches back down into a more-derived type) that
// lets a manipulator merge one-shot or persistent "camera actions" - a fly-to animation, a shake,
// agent-driven nudges -- onto itself, without a caller needing a second manipulator object or to
// know/care which concrete manipulator type is in play. osgx::ActionsManipulator<osgGA::TrackballManipulator>
// genuinely IS a TrackballManipulator: every interaction method (handle, home, getMatrix, setNode,
// ...) is inherited directly, not forwarded through a held ref_ptr.
//
// Actions are plain osg::Callback subclasses (see osgx/CameraActions.hpp for FlyToCallback/
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

// Type-erases ActionsManipulator<Base>'s extra surface so a generic osg::Callback - which only
// ever receives a plain osg::Object* - can reach back into "whatever manipulator it's attached
// to" without needing to know Base. dynamic_cast across this is safe regardless of Base: OSG never
// disables RTTI, and osg::Object has a virtual destructor, so it stays live throughout.
class CameraActionsInterface {
public:
	virtual double currentTime() const = 0;
	virtual void addUpdateCameraCallback(osg::Callback* cb, bool runOnce=false) = 0;
	virtual void removeUpdateCameraCallback(osg::Callback* cb) = 0;

protected:
	virtual ~CameraActionsInterface() {}
};

template<OSGCameraManipulator Base=osgGA::TrackballManipulator>
class ActionsManipulator: public Base, public CameraActionsInterface {
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

	// Peeks at FRAME events to cache the current time for actions to read via currentTime() --
	// matches how OSG's own animated manipulators source time (from the FRAME event, not a polled
	// osg::Timer), and keeps action timing deterministically testable later by injecting FRAME
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
