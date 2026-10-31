#include "osgx/debug/Annotation.hpp"
#include "osgx/debug/Trace.hpp"

#include "osgx/Callbacks.hpp"

#include <string>
#include <string_view>

namespace osgx::debug {

namespace {

constexpr Location ANNOTATION("annotation", Phase::DRAW);
constexpr Location MESSAGE("message", Phase::DRAW);
constexpr Location DRIVER_MESSAGE("driver message", Phase::DRAW);

}

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

	// The GL's echoes of the application's own groups and inserted messages (already traced by
	// pushGroup()/messageInsert()).
	if(
		typ == Type::PUSH_GROUP ||
		typ == Type::POP_GROUP ||
		src == Source::APPLICATION ||
		src == Source::THIRD_PARTY
	) return;

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

	threadTraceBuffer().recordText(TraceRecord::Kind::INSTANT, DRIVER_MESSAGE, msg);
}

void pushGroup(Source source, GLuint id, std::string_view message) {
	if(!_pushGroup) return;

	_pushGroup(
		static_cast<std::underlying_type_t<Source>>(source),
		id,
		static_cast<GLsizei>(message.size()),
		message.data()
	);
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
	std::string_view message
) {
	if(!_messageInsert) return;

	_messageInsert(source, type, id, severity, static_cast<GLsizei>(message.size()), message.data());
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

	if(f) *func = reinterpret_cast<T>(f);

	else notify("osgx::debug | FAILED to bind '", name, "'");
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

void pushGroup(Source source, GLuint id, std::string_view message) {
	threadTraceBuffer().recordText(TraceRecord::Kind::ENTER, ANNOTATION, message);

	detail::pushGroup(source, id, message);
}

void pushGroup(GLuint id, std::string_view message) {
	pushGroup(Source::APPLICATION, id, message);
}

void popGroup() {
	detail::popGroup();

	threadTraceBuffer().recordText(TraceRecord::Kind::LEAVE, ANNOTATION, {});
}

void messageInsert(
	Source source,
	Type type,
	GLuint id,
	Severity severity,
	std::string_view message
) {
	threadTraceBuffer().recordText(TraceRecord::Kind::INSTANT, MESSAGE, message);

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
	std::string_view message
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
	if(_active) return;

	pushGroup(_source, _id, _message);

	_active = true;
}

void AnnotationGroup::end() {
	if(!_active) return;

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

}
