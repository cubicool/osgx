#include "osgx/Debug.hpp"

#include <algorithm>
#include <string_view>

namespace osgx::debug {

namespace detail {

const char* toString(Source s) {
	switch(s) {
		case Source::DONT_CARE: return "DONT_CARE";
		case Source::API: return "API";
		case Source::APPLICATION: return "APPLICATION";
		case Source::OTHER: return "OTHER";
		case Source::SHADER_COMPILER: return "SHADER_COMPILER";
		case Source::THIRD_PARTY: return "THIRD_PARTY";
		case Source::WINDOW_SYSTEM: return "WINDOW_SYSTEM";
	}

	return "UNKNOWN_SOURCE";
}

const char* toString(Type t) {
	switch(t) {
		case Type::DONT_CARE: return "DONT_CARE";
		case Type::DEPRECATED_BEHAVIOR: return "DEPRECATED_BEHAVIOR";
		case Type::ERROR: return "ERROR";
		case Type::MARKER: return "MARKER";
		case Type::OTHER: return "OTHER";
		case Type::PERFORMANCE: return "PERFORMANCE";
		case Type::POP_GROUP: return "POP_GROUP";
		case Type::PORTABILITY: return "PORTABILITY";
		case Type::PUSH_GROUP: return "PUSH_GROUP";
		case Type::UNDEFINED_BEHAVIOR: return "UNDEFINED_BEHAVIOR";
	}

	return "UNKNOWN_TYPE";
}

const char* toString(Severity s) {
	switch(s) {
		case Severity::DONT_CARE: return "DONT_CARE";
		case Severity::HIGH: return "HIGH";
		case Severity::LOW: return "LOW";
		case Severity::MEDIUM: return "MEDIUM";
		case Severity::NOTIFICATION: return "NOTIFICATION";
	}

	return "UNKNOWN_SEVERITY";
}

glPushDebugGroupFunc _pushGroup = nullptr;
glPopDebugGroupFunc _popGroup = nullptr;
glDebugMessageInsertFunc _messageInsert = nullptr;
glDebugMessageCallbackFunc _messageCallback = nullptr;
glDebugMessageControlFunc _messageControl = nullptr;

std::function<void(std::string_view)> _sink = [](std::string_view s) {
	osg::notify(osg::NOTICE) << s << std::endl;
};

void APIENTRY defaultCallback(
	GLenum source,
	GLenum type,
	GLuint id,
	GLenum severity,
	GLsizei length,
	const GLchar* message,
	const void* userParam
) {
	const auto src = static_cast<Source>(source);
	const auto typ = static_cast<Type>(type);
	const auto sev = static_cast<Severity>(severity);

	std::string msg;

	if(message) {
		if(length >= 0) msg.assign(message, static_cast<size_t>(length));
		else msg = message;
	}

	std::ostringstream oss;

	oss << "osgx::debug | source=" << toString(src)
		<< " type=" << toString(typ)
		<< " severity=" << toString(sev)
		<< " id=" << id
		<< " | " << msg;

	_sink(oss.str());
}

void pushGroup(Source source, GLuint id, const std::string& message) {
	if(!_pushGroup) return;

	_pushGroup(
		static_cast<std::underlying_type_t<Source>>(source),
		id,
		-1,
		message.c_str()
	);

	notify("osgx::debug::pushGroup | ", message);
}

void popGroup() {
	if(!_popGroup) return;

	_popGroup();
}

void messageInsert(
	GLenum source,
	GLenum type,
	GLuint id,
	GLenum severity,
	const std::string& message
) {
	if(!_messageInsert) return;

	_messageInsert(source, type, id, severity, -1, message.c_str());

	notify("osgx::debug::messageInsert | ", message);
}

void setCallback(GLDEBUGPROC callback, const void* userParam) {
	if(!_messageCallback) return;

	_messageCallback(callback, userParam);
}

void clearCallback() {
	setCallback(nullptr, nullptr);
}

void enableDebugOutput(bool synchronous) {
	if(!_messageCallback) return;

	glEnable(GL_DEBUG_OUTPUT);

	if(synchronous) glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	else glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
}

void disableDebugOutput() {
	if(!_messageCallback) return;

	glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	glDisable(GL_DEBUG_OUTPUT);
}

void installDefaultCallback(bool synchronous) {
	if(!_messageCallback) return;

	enableDebugOutput(synchronous);
	setCallback(defaultCallback, nullptr);
}

void messageControl(
	GLenum source,
	GLenum type,
	GLenum severity,
	bool enabled,
	GLsizei count,
	const GLuint* ids
) {
	if(!_messageControl) return;

	_messageControl(
		source,
		type,
		severity,
		count,
		ids,
		enabled ? GL_TRUE : GL_FALSE
	);
}

// File-local: only initialize() below needs this, so it never appears in the public header.
template<typename T>
void setupFunction(const std::string& name, T* func) {
	void* f = osg::getGLExtensionFuncPtr(name.c_str());

	if(f) {
		*func = reinterpret_cast<T>(f);

		notify("osgx::debug | Bound function '", name, "' to @", (void*)(*func));
	}

	else notify("osgx::debug | FAILED to bind '", name, "'");
}

FrameAccumulator::Stats FrameAccumulator::snapshot() const {
	std::lock_guard<std::mutex> lock(*statsMutex);

	return stats;
}

FrameAccumulator::Entry FrameAccumulator::alloc(osg::GLExtensions* ext) {
	if(!freeList.empty()) {
		auto e = std::move(freeList.back());

		freeList.pop_back();

		return e;
	}

	Entry e;

	ext->glGenQueries(1, &e.begin);
	ext->glGenQueries(1, &e.end);

	return e;
}

void FrameAccumulator::push(Entry e) {
	pending.push_back(std::move(e));
}

void FrameAccumulator::markCull(const std::string& path, CullState state, unsigned int frameNum) {
	std::lock_guard<std::mutex> lock(*statsMutex);
	auto& s = stats[path];

	s.cullState = state;
	s.cullFrame = frameNum;
}

void FrameAccumulator::drain(
	osg::GLExtensions* ext,
	size_t sampleWindow,
	size_t printEvery,
	unsigned int frameNum
) {
	_drain(pending, ext, sampleWindow, printEvery, frameNum);
}

void FrameAccumulator::swap_and_drain(
	osg::GLExtensions* ext,
	size_t sampleWindow,
	size_t printEvery,
	unsigned int frameNum
) {
	_drain(ready, ext, sampleWindow, printEvery, frameNum);

	std::swap(pending, ready);
}

void FrameAccumulator::_drain(
	std::vector<Entry>& source,
	osg::GLExtensions* ext,
	size_t sampleWindow,
	size_t printEvery,
	unsigned int frameNum
) {
	for(auto& e : source) {
		GLuint64 t0 = 0, t1 = 0;

		ext->glGetQueryObjectui64v(e.begin, GL_QUERY_RESULT, &t0);
		ext->glGetQueryObjectui64v(e.end, GL_QUERY_RESULT, &t1);

		const GLuint64 gpuNs = t1 - t0;
		bool shouldPrint = false;
		GLuint64 avgNs = 0;

		{
			std::lock_guard<std::mutex> lock(*statsMutex);
			auto& s = stats[e.path];

			s.gpuBuffer.add(gpuNs, sampleWindow);
			s.samplesSincePrint++;

			if(printEvery > 0 && s.samplesSincePrint >= printEvery) {
				s.samplesSincePrint = 0;
				shouldPrint = true;
				avgNs = s.gpuBuffer.average(printEvery);
			}
		}

		// Kept outside the lock - notify()/stdout shouldn't hold up the cull thread.
		if(shouldPrint) {
			// TODO: This is annoyingly AWFUL and should be fixed; SOON!
			notify(
				"osgx::debug::Profiler | [", e.path, "] GPU: ", gpuNs / 1000u, "us",
				" | avg: ", avgNs / 1000u, "us",
				" Frame: ", frameNum
			);
		}

		freeList.push_back(std::move(e));
	}

	source.clear();
}

osg::buffered_object<FrameAccumulator> _accumulators;

std::string cameraQualifiedPath(const osg::Camera* camera, const std::string& name) {
	if(!camera) return name;

	const auto& cameraName = camera->getName();

	std::string label = cameraName.empty()
		? camera->className() + std::string("@") + std::to_string(reinterpret_cast<std::uintptr_t>(camera))
		: cameraName;

	return label + "/" + name;
}

osg::Camera::DrawCallback* getCameraDrawCallback(osg::Camera* camera, CameraDrawCallbackSlot slot) {
	switch(slot) {
		case CameraDrawCallbackSlot::PRE_DRAW: return camera->getPreDrawCallback();
		case CameraDrawCallbackSlot::POST_DRAW: return camera->getPostDrawCallback();
		case CameraDrawCallbackSlot::FINAL_DRAW: return camera->getFinalDrawCallback();
	}

	return nullptr;
}

void setCameraDrawCallback(
	osg::Camera* camera,
	CameraDrawCallbackSlot slot,
	osg::Camera::DrawCallback* cb
) {
	switch(slot) {
		case CameraDrawCallbackSlot::PRE_DRAW:
			camera->setPreDrawCallback(cb); break;

		case CameraDrawCallbackSlot::POST_DRAW:
			camera->setPostDrawCallback(cb); break;

		case CameraDrawCallbackSlot::FINAL_DRAW:
			camera->setFinalDrawCallback(cb); break;
	}
}

}

detail::FrameAccumulator::Stats profilerStats(unsigned int contextID) {
	return detail::_accumulators[contextID].snapshot();
}

void pushGroup(Source source, GLuint id, const std::string& message) {
	detail::pushGroup(source, id, message);
}

void pushGroup(GLuint id, const std::string& message) {
	pushGroup(Source::APPLICATION, id, message);
}

void popGroup() {
	detail::popGroup();
}

void messageInsert(
	Source source,
	Type type,
	GLuint id,
	Severity severity,
	const std::string& message
) {
	detail::messageInsert(
		static_cast<std::underlying_type_t<Source>>(source),
		static_cast<std::underlying_type_t<Type>>(type),
		id,
		static_cast<std::underlying_type_t<Severity>>(severity),
		message
	);
}

void messageInsert(
	Type type,
	GLuint id,
	Severity severity,
	const std::string& message
) {
	messageInsert(Source::APPLICATION, type, id, severity, message);
}

void setCallback(GLDEBUGPROC callback, const void* userParam) {
	detail::setCallback(callback, userParam);
}

void clearCallback() {
	detail::clearCallback();
}

void installDefaultCallback(bool synchronous) {
	detail::installDefaultCallback(synchronous);
}

void setSink(std::function<void(std::string_view)> sink) {
	detail::_sink = std::move(sink);
}

void enableDebugOutput(bool synchronous) {
	detail::enableDebugOutput(synchronous);
}

void disableDebugOutput() {
	detail::disableDebugOutput();
}

void messageControl(
	Source source,
	Type type,
	Severity severity,
	bool enabled,
	GLsizei count,
	const GLuint* ids
) {
	detail::messageControl(
		static_cast<std::underlying_type_t<Source>>(source),
		static_cast<std::underlying_type_t<Type>>(type),
		static_cast<std::underlying_type_t<Severity>>(severity),
		enabled,
		count,
		ids
	);
}

void messageControl(Source source, Type type, Severity severity, bool enabled) {
	messageControl(source, type, severity, enabled, 0, nullptr);
}

void initialize(osg::GraphicsContext* gc) {
	if(osg::isGLExtensionSupported(gc->getState()->getContextID(), "GL_KHR_debug")) {
		detail::setupFunction("glPushDebugGroup", &detail::_pushGroup);
		detail::setupFunction("glPopDebugGroup", &detail::_popGroup);
		detail::setupFunction("glDebugMessageInsert", &detail::_messageInsert);
		detail::setupFunction("glDebugMessageCallback", &detail::_messageCallback);
		detail::setupFunction("glDebugMessageControl", &detail::_messageControl);
	}
}

void deinitialize(bool disableOutput) {
	clearCallback();

	if(disableOutput && detail::_messageCallback) disableDebugOutput();

	detail::_pushGroup = nullptr;
	detail::_popGroup = nullptr;
	detail::_messageInsert = nullptr;
	detail::_messageCallback = nullptr;
	detail::_messageControl = nullptr;
}

void AnnotationGroup::begin() {
	_active = detail::_pushGroup != nullptr;

	if(_active) pushGroup(_source, _id, _message);

	if(_measureTime) _start = osg::Timer::instance()->tick();
}

void AnnotationGroup::end() {
	if(!_active) return;

	if(_measureTime) {
		auto stop = osg::Timer::instance()->tick();
		auto dt = stop - _start;
		// TODO: This is more correct?
		// auto dt = osg::Timer::instance()->delta_u(_start, stop);

		messageInsert(
			Type::PERFORMANCE,
			_id,
			Severity::NOTIFICATION,
			_message + " took " + std::to_string(dt) + "us"
		);
	}

	popGroup();

	_active = false;
}

void appendCameraDrawCallback(
	osg::Camera* camera,
	CameraDrawCallbackSlot slot,
	osg::Camera::DrawCallback* cb
) {
	auto* existing = detail::getCameraDrawCallback(camera, slot);

	if(!existing) {
		detail::setCameraDrawCallback(camera, slot, cb);

		return;
	}

	if(auto* group = dynamic_cast<osgx::CameraDrawCallbacksGroup*>(existing)) {
		group->add(cb);

		return;
	}

	detail::setCameraDrawCallback(
		camera,
		slot,
		new osgx::CameraDrawCallbacksGroup({existing, cb})
	);
}

void prependCameraDrawCallback(
	osg::Camera* camera,
	CameraDrawCallbackSlot slot,
	osg::Camera::DrawCallback* cb
) {
	auto* existing = detail::getCameraDrawCallback(camera, slot);

	if(!existing) {
		detail::setCameraDrawCallback(camera, slot, cb);
		return;
	}

	detail::setCameraDrawCallback(
		camera,
		slot,
		new osgx::CameraDrawCallbacksGroup({cb, existing})
	);
}

bool ProfilerCullCallback::cull(osg::NodeVisitor* nv, osg::Drawable* drawable, osg::RenderInfo* ri) const {
	const auto frameNum = nv && nv->getFrameStamp()
		? nv->getFrameStamp()->getFrameNumber()
		: 0u
	;

	const auto contextID = ri && ri->getState()
		? ri->getState()->getContextID()
		: 0u
	;

	auto* cv = nv ? nv->asCullVisitor() : nullptr;
	const auto& leafName = _name.empty() && drawable ? drawable->getName() : _name;
	const auto path = detail::cameraQualifiedPath(cv ? cv->getCurrentCamera() : nullptr, leafName);
	bool callbackCulled = false;

	if(_cb.valid()) {
		if(auto* dcb = _cb->asDrawableCullCallback()) {
			callbackCulled = dcb->cull(nv, drawable, ri);
		}

		else _cb->run(drawable, nv);
	}

	const bool boundsCulled = drawable && cv && drawable->isCullingActive()
		&& cv->isCulled(drawable->getBoundingBox())
	;

	detail::_accumulators[contextID].markCull(
		path,
		(callbackCulled || boundsCulled) ? CullState::CULLED : CullState::VISIBLE,
		frameNum
	);

	return callbackCulled;
}

bool FrameByFrameViewer::EventHandler::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) {
	auto* viewer = dynamic_cast<FrameByFrameViewer*>(&aa);

	if(viewer && ea.getEventType() == osgGA::GUIEventAdapter::KEYUP) {
		if(ea.getKey() == 'n') {
			viewer->requestRender();

			return true;
		}
	}

	return false;
}

void FrameByFrameViewer::_ensureInitialized() {
	if(_firstFrame) {
		viewerInit();

		if(!isRealized()) realize();

		_firstFrame = false;
	}
}

void FrameByFrameViewer::_installAnnotationCallbacks() {
	if(_annotationCallbacksInstalled) return;

	auto* camera = getCamera();

	auto frameAnnotation = osgx::make_ref<AnnotationGroup>(
		0u,
		"Frame",
		Source::APPLICATION,
		true
	);

	appendCameraDrawCallback(
		camera,
		CameraDrawCallbackSlot::PRE_DRAW,
		new AnnotationBeginCallback(frameAnnotation)
	);

	appendCameraDrawCallback(
		camera,
		CameraDrawCallbackSlot::FINAL_DRAW,
		new AnnotationEndCallback(frameAnnotation)
	);

	_annotationCallbacksInstalled = true;
}

int FrameByFrameViewer::run() {
	while(!done()) {
		_ensureInitialized();
		_installAnnotationCallbacks();

		advance();
		eventTraversal();

		if(_render.load(std::memory_order_acquire)) {
			_count++;

			getFrameStamp()->setFrameNumber(_count);

			double wallTime = osg::Timer::instance()->delta_s(
				_startTick,
				osg::Timer::instance()->tick()
			);

			detail::notify(
				"osgx::debug::FrameByFrameViewer | render #", _count,
				" @", wallTime, "s"
			);

			auto [_ut, ut] = osgx::call([this]() { updateTraversal(); });

			detail::notify("osgx::debug::FrameByFrameViewer | Update took ", ut, "us");

			// Below are exactly what the typical `osgViewer::Viewer::renderingTraversals()`
			// method does.
			//
			// - Calculate frame time/number.
			// - Collect stats (if enabled).
			// - Iterate over getScenes().
			//   - Call scene.DatabasePager.signalBeginFrame().
			//   - Call scene.ImagePager.signalBeginFrame().
			//   - Compute bounds of scene.
			// - Iterate over getCameras().
			//   - Call camera.getRenderer().cull().
			// - Iterate over getContexts(), defined in subclass (Viewer/CompositeViewer).
			//   - Make context thread active.
			//   - Call context.runOperations().
			//     - Iterate over all context cameras.
			//       - Call camera.getRenderer()(context).
			//         - osgViewer::Renderer calls either cull_draw() or draw(), FINALLY
			//           leading to our Drawable! The order of function call is:
			//             - SceneView::draw()
			//             - RenderBin::draw()
			// - Iterate over getContexts().
			//   - Make context thread active.
			//   - Call context.swapBuffers().
			// - Iterate over getScenes().
			//   - Call scene.DatabasePager.signalEndFrame().
			//   - Call scene.ImagePager.signalEndFrame().
			// - Update stats (if enabled).
			renderingTraversals();

			_render.store(false, std::memory_order_release);
		}

		OpenThreads::Thread::microSleep(_POLL_INTERVAL_US);
	}

	return 0;
}

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
