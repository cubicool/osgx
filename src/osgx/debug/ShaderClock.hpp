#pragma once

#include "osgx/Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Drawable>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/buffered_value>

OSGX_ENABLE_WARNINGS

namespace osgx::debug {

// GPU load simulator backed by GL_ARB_shader_clock. It renders a private fullscreen triangle to
// a 1x1 FBO and spins in its fragment shader for a caller-controlled duration. The Drawable is
// safe to add directly to a scene graph and restores every GL binding it changes after each draw.
//
// DYNAMIC calibration measures the implementation-defined shader-clock rate lazily with GPU
// timestamp queries. STATIC calibration uses a caller-provided rate and never reads query results.
enum class ClockCalibration {
	DYNAMIC,
	STATIC
};

class ShaderClockDrawable: public osg::Drawable {
public:
	explicit ShaderClockDrawable(ClockCalibration mode=ClockCalibration::DYNAMIC);

	void setDuration(double milliseconds) { _durationMs = milliseconds; }
	double getDuration() const { return _durationMs; }

	ClockCalibration getCalibrationMode() const { return _mode; }

	void setTicksPerMillisecond(double ticksPerMs) { _staticTicksPerMs = ticksPerMs; }
	double getStaticTicksPerMillisecond() const { return _staticTicksPerMs; }
	double getTicksPerMillisecond(unsigned int contextID=0) const;

	// DYNAMIC mode only. Marks every initialized context for calibration on its next draw.
	void recalibrate();

	// Zero calibrates once. A positive interval recalibrates DYNAMIC mode periodically.
	void setRecalibrationInterval(double seconds) { _recalInterval = seconds; }
	double getRecalibrationInterval() const { return _recalInterval; }

	// Zero calibrates once. A positive interval recalibrates DYNAMIC mode after this many graphics
	// frames. If both intervals are non-zero, whichever expires first triggers recalibration.
	void setRecalibrationFrameInterval(unsigned int frames) { _recalFrameInterval = frames; }
	unsigned int getRecalibrationFrameInterval() const { return _recalFrameInterval; }

	// Returns false when the first draw under a context found GL_ARB_shader_clock unavailable.
	bool isSupported(unsigned int contextID) const;

	void drawImplementation(osg::RenderInfo& ri) const override;
	void releaseGLObjects(osg::State* state=nullptr) const override;

protected:
	~ShaderClockDrawable() override = default;

private:
	struct PerContextState {
		bool initialized = false;
		bool supported = false;
		bool calibrated = false;

		GLuint fbo = 0;
		GLuint colorRenderbuffer = 0;
		GLuint vao = 0;
		GLuint program = 0;
		GLint targetTicksLoc = -1;

		double ticksPerMs = 0.0;
		double overheadNs = 0.0;
		double lastCalibration = 0.0;
		unsigned int lastCalibrationFrame = 0;
	};

	void _init(osg::State& state, PerContextState& pcs) const;
	void _calibrate(osg::State& state, PerContextState& pcs) const;
	void _release(unsigned int contextID, PerContextState& pcs) const;

	ClockCalibration _mode;
	double _durationMs{0.0};
	double _staticTicksPerMs{0.0};
	double _recalInterval{0.0};
	unsigned int _recalFrameInterval = 0;

	mutable osg::buffered_object<PerContextState> _state;
};

}
