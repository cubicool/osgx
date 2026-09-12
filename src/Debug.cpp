#include "osgx/Debug.hpp"

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

}
