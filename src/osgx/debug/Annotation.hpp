#pragma once

#include "osgx/Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/GLExtensions>
#include <osg/GraphicsContext>
#include <osg/GraphicsThread>
#include <osg/Referenced>
#include <osg/Timer>

OSGX_ENABLE_WARNINGS

#include <functional>
#include <sstream>
#include <string>
#include <string_view>

// TODO: Make some macro wrappers for pushGroup/insertMessage that use __FUNCTION__, __FILE__, etc.
// TODO: pushGroup/insertMessage accept an "id", which we probably should manage automatically.

namespace osgx::debug {

using namespace osgx::literals;

enum class Severity: GLenum {
	DONT_CARE = 0x1100,
	HIGH = 0x9146,
	LOW = 0x9148,
	MEDIUM = 0x9147,
	NOTIFICATION = 0x826B
};

// TODO: The default "source" (for pushGroup/messageInsert) is APPLICATION; does this make sense?
// What are the others even FOR!?
enum class Source: GLenum {
	DONT_CARE = 0x1100,
	API = 0x8246,
	APPLICATION = 0x824A,
	OTHER = 0x824B,
	SHADER_COMPILER = 0x8248,
	THIRD_PARTY = 0x8249,
	WINDOW_SYSTEM = 0x8247
};

enum class Type: GLenum {
	DONT_CARE = 0x1100,
	DEPRECATED_BEHAVIOR = 0x824D,
	ERROR = 0x824C,
	MARKER = 0x8268,
	OTHER = 0x8251,
	PERFORMANCE = 0x8250,
	POP_GROUP = 0x826A,
	PORTABILITY = 0x824F,
	PUSH_GROUP = 0x8269,
	UNDEFINED_BEHAVIOR = 0x824E
};

namespace detail {
	const char* toString(Source s);
	const char* toString(Type t);
	const char* toString(Severity s);
}

namespace detail {
	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glPushDebugGroup.xhtml
	using glPushDebugGroupFunc = void(APIENTRYP)(GLenum, GLuint, GLsizei, const char*);

	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glPopDebugGroup.xhtml
	using glPopDebugGroupFunc = void(APIENTRYP)();

	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDebugMessageInsert.xhtml
	using glDebugMessageInsertFunc = void(APIENTRYP)(GLenum, GLenum, GLuint, GLenum, GLsizei, const char*);

	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDebugMessageCallback.xhtml
	using glDebugMessageCallbackFunc = void(APIENTRYP)(GLDEBUGPROC, const void*);

	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDebugMessageControl.xhtml
	using glDebugMessageControlFunc = void(APIENTRYP)(
		GLenum source,
		GLenum type,
		GLenum severity,
		GLsizei count,
		const GLuint* ids,
		GLboolean enabled
	);

	// Defined once in Debug.cpp. Deliberately NOT `inline` variables: an `inline` namespace-
	// scope variable only merges into one definition within a single link unit. Any separately
	// dlopen()'d module (an osgDB plugin, most of which load via RTLD_LOCAL) that included this
	// header directly used to get its own private copy of this state, disconnected from every
	// other consumer's - e.g. a ProfilerVisitor running in one module writing into a
	// FrameAccumulator that profilerStats() in another module could never see. `extern` +
	// exactly one definition in the compiled osgx library fixes that: every consumer that links
	// osgx::osgx resolves to the same already-loaded symbol.
	extern glPushDebugGroupFunc _pushGroup;
	extern glPopDebugGroupFunc _popGroup;
	extern glDebugMessageInsertFunc _messageInsert;
	extern glDebugMessageCallbackFunc _messageCallback;
	extern glDebugMessageControlFunc _messageControl;

	// All osgx::debug log output - internal diagnostics, GL annotation mirrors, driver
	// messages - flows through this single sink. Replace it to redirect to spdlog,
	// Qt logging, an ImGui scrollback, etc. Default: osg::notify(NOTICE).
	extern std::function<void(std::string_view)> _sink;

	inline void notify(const auto&... args) {
		std::ostringstream oss;
		((oss << args), ...);
		_sink(oss.str());
	}

	void APIENTRY defaultCallback(
		GLenum source,
		GLenum type,
		GLuint id,
		GLenum severity,
		GLsizei length,
		const GLchar* message,
		const void* userParam
	);

	void pushGroup(Source source, GLuint id, const std::string& message);
	void popGroup();

	void messageInsert(
		GLenum source,
		GLenum type,
		GLuint id,
		GLenum severity,
		const std::string& message
	);

	void setCallback(GLDEBUGPROC callback, const void* userParam=nullptr);
	void clearCallback();
	void enableDebugOutput(bool synchronous=true);
	void disableDebugOutput();
	void installDefaultCallback(bool synchronous=true);

	void messageControl(
		GLenum source,
		GLenum type,
		GLenum severity,
		bool enabled,
		GLsizei count=0,
		const GLuint* ids=nullptr
	);

}

void pushGroup(Source source, GLuint id, const std::string& message);
void pushGroup(GLuint id, const std::string& message);
void popGroup();

void messageInsert(
	Source source,
	Type type,
	GLuint id,
	Severity severity,
	const std::string& message
);

void messageInsert(
	Type type,
	GLuint id,
	Severity severity,
	const std::string& message
);

// TODO: More messageInsert() wrappers for Type/Severity.

void setCallback(GLDEBUGPROC callback, const void* userParam=nullptr);
void clearCallback();
void installDefaultCallback(bool synchronous=true);

// Replace the log sink for all osgx::debug output (internal diagnostics, GL annotation
// mirrors, driver messages). The default writes to osg::notify(NOTICE).
void setSink(std::function<void(std::string_view)> sink);

void enableDebugOutput(bool synchronous=true);
void disableDebugOutput();

void messageControl(
	Source source,
	Type type,
	Severity severity,
	bool enabled,
	GLsizei count,
	const GLuint* ids=nullptr
);

void messageControl(Source source, Type type, Severity severity, bool enabled);

// TODO: Add a check for whether we're already initialized.
void initialize(osg::GraphicsContext* gc);
void deinitialize(bool disableOutput=true);

// Annotations are camera-scoped: a PRE_DRAW/FINAL_DRAW pair brackets everything that camera
// renders. Sub-groups within a single camera's scene graph cannot be individually annotated this
// way; for per-group bracketing, each group needs its own camera (RTT). For per-drawable bracketing
// use AnnotationScope inside a DrawCallback instead.
//
// Choosing the right tool:
//
// AnnotationScope - begin and end are in the SAME scope (e.g. inside a single drawImplementation call).
// RAII; use on the stack.
//
// AnnotationGroup - begin and end must live in SEPARATE callbacks. Its sole purpose is to share
// state across AnnotationBeginCallback (PRE_DRAW) and AnnotationEndCallback (FINAL_DRAW) so the
// push/pop remain matched across the camera's frame boundary. Not a general-purpose annotation
// type.
//
// All calls require a current GL context; never call begin()/end() from application-side C++ code
// outside a draw callback.
class AnnotationGroup: public osg::Referenced {
public:
	AnnotationGroup(
		GLuint id,
		std::string_view message,
		Source source=Source::APPLICATION,
		bool measureTime=false
	):
	_id(id),
	_source(source),
	_message(message),
	_measureTime(measureTime) {}

	void begin();
	void end();

private:
	GLuint _id;
	Source _source;
	std::string _message;

	bool _active = false;
	bool _measureTime = false;

	osg::Timer_t _start{};
};

// RAII wrapper: opens the group on construction, closes on destruction.
// Safe on the stack inside a draw callback; do not use from application C++ code.
class AnnotationScope {
public:
	AnnotationScope(
		GLuint id,
		std::string_view message,
		Source source=Source::APPLICATION,
		bool measureTime=false
	):
	_state(id, message, source, measureTime) {
		_state.begin();
	}

	~AnnotationScope() {
		_state.end();
	}

	AnnotationScope(const AnnotationScope&) = delete;
	AnnotationScope& operator=(const AnnotationScope&) = delete;

private:
	AnnotationGroup _state;
};

enum class CameraDrawCallbackSlot {
	PRE_DRAW,
	POST_DRAW,
	FINAL_DRAW
};

namespace detail {
	osg::Camera::DrawCallback* getCameraDrawCallback(osg::Camera* camera, CameraDrawCallbackSlot slot);

	void setCameraDrawCallback(
		osg::Camera* camera,
		CameraDrawCallbackSlot slot,
		osg::Camera::DrawCallback* cb
	);
}

void appendCameraDrawCallback(osg::Camera* camera, CameraDrawCallbackSlot slot, osg::Camera::DrawCallback* cb);
void prependCameraDrawCallback(osg::Camera* camera, CameraDrawCallbackSlot slot, osg::Camera::DrawCallback* cb);

// Fires AnnotationGroup::begin() from a camera draw slot (GL context current).
class AnnotationBeginCallback: public osg::Camera::DrawCallback {
public:
	using Annotation = osg::ref_ptr<AnnotationGroup>;

	AnnotationBeginCallback(
		GLuint id,
		std::string_view message,
		Source source=Source::APPLICATION,
		bool measureTime=false
	):
	_annotation(new AnnotationGroup(id, message, source, measureTime)) {}

	explicit AnnotationBeginCallback(Annotation annotation):
	_annotation(std::move(annotation)) {}

	void operator()(osg::RenderInfo&) const override {
		_annotation->begin();
	}

private:
	Annotation _annotation;
};

// Fires AnnotationGroup::end() from a camera draw slot (GL context current).
class AnnotationEndCallback: public osg::Camera::DrawCallback {
public:
	using Annotation = osg::ref_ptr<AnnotationGroup>;

	AnnotationEndCallback(
		GLuint id,
		std::string_view message,
		Source source=Source::APPLICATION,
		bool measureTime=false
	):
	_annotation(new AnnotationGroup(id, message, source, measureTime)) {}

	explicit AnnotationEndCallback(Annotation annotation):
	_annotation(std::move(annotation)) {}

	void operator()(osg::RenderInfo&) const override {
		_annotation->end();
	}

private:
	Annotation _annotation;
};

/* static const auto DEBUG_EXTENSIONS = std::vector<std::string>{
	"GL_KHR_debug",
	"GL_ARB_debug_output",
	"GL_EXT_debug_marker",
	"GL_EXT_debug_label",
	"GL_AMD_debug_output",
}; */

class GraphicsOperation: public osg::GraphicsOperation {
public:
	GraphicsOperation():
	osg::Referenced(true),
	osg::GraphicsOperation("osgx::debug::initialize Operation", false) {}

	virtual void operator()(osg::GraphicsContext* gc) {
		initialize(gc);
	}
};

}
