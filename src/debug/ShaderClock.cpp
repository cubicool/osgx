#include "osgx/debug/ShaderClock.hpp"

#include "osgx/debug/Annotation.hpp"

#include <algorithm>

namespace osgx::debug {

namespace {

constexpr double CALIBRATION_TARGET_SECONDS = 0.002;
constexpr int CALIBRATION_MAX_ATTEMPTS = 16;
constexpr GLuint64 CALIBRATION_INITIAL_TICKS = 1u << 16;

const char* const SHADER_CLOCK_VERT_SRC = R"FOO(
#version 460 core

void main() {
	vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)FOO";

const char* const SHADER_CLOCK_FRAG_SRC = R"FOO(
#version 460 core
#extension GL_ARB_shader_clock : require

uniform uint osgx_TargetTicks;
out vec4 osgx_FragColor;

void main() {
	uint t0 = clock2x32ARB().x;
	uint elapsed = 0u;
	while(elapsed < osgx_TargetTicks) elapsed = clock2x32ARB().x - t0;
	osgx_FragColor = vec4(float(elapsed), 0.0, 0.0, 1.0);
}
)FOO";

bool compileShader(osg::GLExtensions* ext, GLuint shader, const char* src) {
	ext->glShaderSource(shader, 1, &src, nullptr);
	ext->glCompileShader(shader);

	GLint ok = GL_FALSE;
	ext->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

	if(!ok) {
		GLchar log[1024];
		GLsizei len = 0;

		ext->glGetShaderInfoLog(shader, sizeof(log), &len, log);
		detail::notify(
			"osgx::debug::ShaderClockDrawable | shader failed to compile: ",
			std::string_view(log, static_cast<std::size_t>(len))
		);
	}

	return ok == GL_TRUE;
}

GLuint64 measureTicks(osg::GLExtensions* ext, GLuint queries[2], GLuint64 ticks, GLint targetTicksLoc) {
	const GLuint clampedTicks = static_cast<GLuint>(std::min<GLuint64>(ticks, 0xFFFFFFFFu));

	ext->glQueryCounter(queries[0], GL_TIMESTAMP);
	ext->glUniform1ui(targetTicksLoc, clampedTicks);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	ext->glQueryCounter(queries[1], GL_TIMESTAMP);

	GLuint64 beginNs = 0;
	GLuint64 endNs = 0;

	ext->glGetQueryObjectui64v(queries[0], GL_QUERY_RESULT, &beginNs);
	ext->glGetQueryObjectui64v(queries[1], GL_QUERY_RESULT, &endNs);

	return endNs - beginNs;
}

struct SavedGLState {
	GLint fbo = 0;
	GLint viewport[4] = {0, 0, 0, 0};
	GLint program = 0;
	GLint vao = 0;

	void save() {
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
		glGetIntegerv(GL_VIEWPORT, viewport);
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
	}

	void restore(osg::GLExtensions* ext) const {
		ext->glBindVertexArray(static_cast<GLuint>(vao));
		ext->glUseProgram(static_cast<GLuint>(program));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		ext->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo));
	}
};

}

ShaderClockDrawable::ShaderClockDrawable(ClockCalibration mode):
_mode(mode) {
	setSupportsDisplayList(false);
	setUseVertexBufferObjects(false);
	setDataVariance(osg::Object::DYNAMIC);
}

double ShaderClockDrawable::getTicksPerMillisecond(unsigned int contextID) const {
	if(contextID >= _state.size()) return 0.0;

	return _state[contextID].ticksPerMs;
}

void ShaderClockDrawable::recalibrate() {
	for(unsigned int i = 0; i < _state.size(); i++) _state[i].calibrated = false;
}

bool ShaderClockDrawable::isSupported(unsigned int contextID) const {
	if(contextID >= _state.size()) return true;

	return _state[contextID].supported;
}

void ShaderClockDrawable::_init(osg::State& state, PerContextState& pcs) const {
	if(pcs.initialized) return;

	pcs.initialized = true;

	const unsigned int contextID = state.getContextID();

	pcs.supported = osg::isGLExtensionSupported(contextID, "GL_ARB_shader_clock");

	if(!pcs.supported) {
		detail::notify(
			"osgx::debug::ShaderClockDrawable | GL_ARB_shader_clock not supported in context ",
			contextID, " - this drawable will never stall"
		);

		return;
	}

	auto* ext = osg::GLExtensions::Get(contextID, true);
	const GLuint vert = ext->glCreateShader(GL_VERTEX_SHADER);
	const GLuint frag = ext->glCreateShader(GL_FRAGMENT_SHADER);
	const bool vertOK = compileShader(ext, vert, SHADER_CLOCK_VERT_SRC);
	const bool fragOK = compileShader(ext, frag, SHADER_CLOCK_FRAG_SRC);

	pcs.program = ext->glCreateProgram();
	ext->glAttachShader(pcs.program, vert);
	ext->glAttachShader(pcs.program, frag);
	ext->glLinkProgram(pcs.program);

	GLint linked = GL_FALSE;
	ext->glGetProgramiv(pcs.program, GL_LINK_STATUS, &linked);
	ext->glDeleteShader(vert);
	ext->glDeleteShader(frag);

	if(!vertOK || !fragOK || !linked) {
		GLchar log[1024];
		GLsizei len = 0;

		ext->glGetProgramInfoLog(pcs.program, sizeof(log), &len, log);
		detail::notify(
			"osgx::debug::ShaderClockDrawable | program failed to link: ",
			std::string_view(log, static_cast<std::size_t>(len))
		);
		ext->glDeleteProgram(pcs.program);
		pcs.program = 0;
		pcs.supported = false;
		return;
	}

	pcs.targetTicksLoc = ext->glGetUniformLocation(pcs.program, "osgx_TargetTicks");
	ext->glGenVertexArrays(1, &pcs.vao);

	SavedGLState saved;
	saved.save();

	ext->glGenRenderbuffers(1, &pcs.colorRenderbuffer);
	ext->glBindRenderbuffer(GL_RENDERBUFFER, pcs.colorRenderbuffer);
	ext->glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
	ext->glGenFramebuffers(1, &pcs.fbo);
	ext->glBindFramebuffer(GL_FRAMEBUFFER, pcs.fbo);
	ext->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, pcs.colorRenderbuffer);

	if(ext->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		detail::notify("osgx::debug::ShaderClockDrawable | 1x1 FBO incomplete in context ", contextID);
		pcs.supported = false;
	}

	saved.restore(ext);
}

void ShaderClockDrawable::_calibrate(osg::State& state, PerContextState& pcs) const {
	const unsigned int contextID = state.getContextID();
	auto* ext = osg::GLExtensions::Get(contextID, true);

	if(!ext->glQueryCounter || !ext->glGetQueryObjectui64v) return;

	SavedGLState saved;
	saved.save();
	ext->glBindFramebuffer(GL_FRAMEBUFFER, pcs.fbo);
	glViewport(0, 0, 1, 1);
	ext->glUseProgram(pcs.program);
	ext->glBindVertexArray(pcs.vao);

	GLuint queries[2] = {0, 0};
	ext->glGenQueries(2, queries);

	GLuint64 ticksB = CALIBRATION_INITIAL_TICKS;
	GLuint64 elapsedB = 0;

	for(int attempt = 0; attempt < CALIBRATION_MAX_ATTEMPTS; attempt++) {
		elapsedB = measureTicks(ext, queries, ticksB, pcs.targetTicksLoc);
		if(static_cast<double>(elapsedB) >= CALIBRATION_TARGET_SECONDS * 1.0e9) break;
		ticksB *= 2;
	}

	const GLuint64 ticksA = std::max<GLuint64>(ticksB / 4, 1);
	const GLuint64 elapsedA = measureTicks(ext, queries, ticksA, pcs.targetTicksLoc);
	ext->glDeleteQueries(2, queries);
	saved.restore(ext);

	if(ticksB > ticksA && elapsedB > elapsedA) {
		const double rate = static_cast<double>(ticksB - ticksA) / static_cast<double>(elapsedB - elapsedA);
		pcs.ticksPerMs = rate * 1.0e6;
		pcs.overheadNs = static_cast<double>(elapsedA) - static_cast<double>(ticksA) / rate;
	}
	else if(elapsedB > 0) {
		pcs.ticksPerMs = static_cast<double>(ticksB) / (static_cast<double>(elapsedB) / 1.0e6);
		pcs.overheadNs = 0.0;
	}

	pcs.calibrated = true;
	pcs.lastCalibration = osg::Timer::instance()->time_s();
	pcs.lastCalibrationFrame = state.getFrameStamp() ? state.getFrameStamp()->getFrameNumber() : 0;
	detail::notify(
		"osgx::debug::ShaderClockDrawable | calibrated context ", contextID, ": ",
		pcs.ticksPerMs, " ticks/ms, ", pcs.overheadNs, "ns fixed overhead"
	);
}

void ShaderClockDrawable::drawImplementation(osg::RenderInfo& ri) const {
	osg::State& state = *ri.getState();
	const unsigned int contextID = state.getContextID();
	PerContextState& pcs = _state[contextID];

	_init(state, pcs);
	if(!pcs.supported) return;

	if(!pcs.calibrated) {
		if(_mode == ClockCalibration::STATIC) {
			pcs.ticksPerMs = _staticTicksPerMs;
			pcs.calibrated = true;
			pcs.lastCalibration = osg::Timer::instance()->time_s();
		}
		else _calibrate(state, pcs);
	}
	else if(_mode == ClockCalibration::DYNAMIC) {
		const double now = osg::Timer::instance()->time_s();
		const auto* frameStamp = state.getFrameStamp();
		const bool timeExpired = _recalInterval > 0.0 && now - pcs.lastCalibration >= _recalInterval;
		const bool frameExpired = _recalFrameInterval > 0 && frameStamp
			&& frameStamp->getFrameNumber() - pcs.lastCalibrationFrame >= _recalFrameInterval;

		if(timeExpired || frameExpired) _calibrate(state, pcs);
	}

	if(pcs.ticksPerMs <= 0.0 || _durationMs <= 0.0) return;

	const double desiredNs = _durationMs * 1.0e6;
	const double rateTicksPerNs = pcs.ticksPerMs / 1.0e6;
	const GLuint targetTicks = static_cast<GLuint>(
		std::clamp((desiredNs - pcs.overheadNs) * rateTicksPerNs, 0.0, static_cast<double>(0xFFFFFFFFu))
	);

	auto* ext = osg::GLExtensions::Get(contextID, true);
	SavedGLState saved;
	saved.save();
	ext->glBindFramebuffer(GL_FRAMEBUFFER, pcs.fbo);
	glViewport(0, 0, 1, 1);
	ext->glUseProgram(pcs.program);
	ext->glUniform1ui(pcs.targetTicksLoc, targetTicks);
	ext->glBindVertexArray(pcs.vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	saved.restore(ext);
}

void ShaderClockDrawable::_release(unsigned int contextID, PerContextState& pcs) const {
	if(!pcs.initialized) return;

	auto* ext = osg::GLExtensions::Get(contextID, false);

	if(ext) {
		if(pcs.program) ext->glDeleteProgram(pcs.program);
		if(pcs.vao) ext->glDeleteVertexArrays(1, &pcs.vao);
		if(pcs.fbo) ext->glDeleteFramebuffers(1, &pcs.fbo);
		if(pcs.colorRenderbuffer) ext->glDeleteRenderbuffers(1, &pcs.colorRenderbuffer);
	}

	pcs = PerContextState();
}

void ShaderClockDrawable::releaseGLObjects(osg::State* state) const {
	osg::Drawable::releaseGLObjects(state);
	if(!state) return;

	const unsigned int contextID = state->getContextID();
	if(contextID >= _state.size()) return;

	_release(contextID, _state[contextID]);
}

}
