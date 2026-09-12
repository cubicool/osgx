#include "osgx-python.hpp"
#include "osgx/Debug.hpp"

namespace osgx_python {

void bind_debug(py::module_& m_debug) {
	py::class_<
		osgx::debug::GraphicsOperation,
		osg::GraphicsOperation,
		osg::ref_ptr<osgx::debug::GraphicsOperation>
	>(
		m_debug,
		"GraphicsOperation",
		"A GraphicsContext realize operation that calls osgx.debug.initialize() - install via "
		"viewer.setRealizeOperation() to bind the GL_KHR_debug function pointers as soon as the "
		"context is ready, instead of calling initialize() by hand."
	)
		.def(py::init<>(), "Constructs the realize operation.")
	;

	py::enum_<osgx::debug::Severity>(
		m_debug,
		"Severity",
		"GL_DEBUG_SEVERITY_* values, for messageControl()/messageInsert()."
	)
		.value("DONT_CARE", osgx::debug::Severity::DONT_CARE)
		.value("HIGH", osgx::debug::Severity::HIGH)
		.value("MEDIUM", osgx::debug::Severity::MEDIUM)
		.value("LOW", osgx::debug::Severity::LOW)
		.value("NOTIFICATION", osgx::debug::Severity::NOTIFICATION)
		.export_values()
	;

	py::enum_<osgx::debug::Source>(
		m_debug,
		"Source",
		"GL_DEBUG_SOURCE_* values, for messageControl()/messageInsert()/pushGroup()."
	)
		.value("DONT_CARE", osgx::debug::Source::DONT_CARE)
		.value("API", osgx::debug::Source::API)
		.value("WINDOW_SYSTEM", osgx::debug::Source::WINDOW_SYSTEM)
		.value("SHADER_COMPILER", osgx::debug::Source::SHADER_COMPILER)
		.value("THIRD_PARTY", osgx::debug::Source::THIRD_PARTY)
		.value("APPLICATION", osgx::debug::Source::APPLICATION)
		.value("OTHER", osgx::debug::Source::OTHER)
		.export_values()
	;

	py::enum_<osgx::debug::Type>(
		m_debug,
		"Type",
		"GL_DEBUG_TYPE_* values, for messageControl()/messageInsert()."
	)
		.value("DONT_CARE", osgx::debug::Type::DONT_CARE)
		.value("ERROR", osgx::debug::Type::ERROR)
		.value("DEPRECATED_BEHAVIOR", osgx::debug::Type::DEPRECATED_BEHAVIOR)
		.value("UNDEFINED_BEHAVIOR", osgx::debug::Type::UNDEFINED_BEHAVIOR)
		.value("PORTABILITY", osgx::debug::Type::PORTABILITY)
		.value("PERFORMANCE", osgx::debug::Type::PERFORMANCE)
		.value("OTHER", osgx::debug::Type::OTHER)
		.value("MARKER", osgx::debug::Type::MARKER)
		.value("PUSH_GROUP", osgx::debug::Type::PUSH_GROUP)
		.value("POP_GROUP", osgx::debug::Type::POP_GROUP)
		.export_values()
	;

	m_debug
		.def(
			"initialize", &osgx::debug::initialize, "gc"_a,
			"Checks for GL_KHR_debug and binds its function pointers on the given, already-"
			"realized GraphicsContext. Every other osgx.debug call is a silent no-op until this "
			"has run for the relevant context."
		)
		.def(
			"deinitialize",
			&osgx::debug::deinitialize,
			"disableOutput"_a=true,
			"Tears down osgx.debug for the current context; disables GL debug output first unless "
			"`disableOutput` is False."
		)
		.def(
			"installDefaultCallback",
			&osgx::debug::installDefaultCallback,
			"synchronous"_a=true,
			"Installs osgx.debug's built-in GL_DEBUG callback (logs to the current sink). Pass "
			"synchronous=False for GL_DEBUG_OUTPUT_SYNCHRONOUS off (messages may arrive off the "
			"issuing thread/frame)."
		)
		.def("clearCallback", &osgx::debug::clearCallback, "Removes any installed GL_DEBUG callback.")
		.def(
			"enableDebugOutput",
			&osgx::debug::enableDebugOutput,
			"synchronous"_a=true,
			"Enables GL_DEBUG_OUTPUT (and GL_DEBUG_OUTPUT_SYNCHRONOUS unless `synchronous` is False)."
		)
		.def("disableDebugOutput", &osgx::debug::disableDebugOutput, "Disables GL_DEBUG_OUTPUT.")
		.def(
			"messageControl",
			py::overload_cast<osgx::debug::Source, osgx::debug::Type, osgx::debug::Severity, bool>(
				&osgx::debug::messageControl
			),
			"source"_a,
			"type"_a,
			"severity"_a,
			"enabled"_a,
			"Enables or disables GL debug messages matching (source, type, severity)."
		)
		.def("messageInsert", py::overload_cast<
			osgx::debug::Source,
			osgx::debug::Type,
			GLuint,
			osgx::debug::Severity,
			const std::string&
		>(&osgx::debug::messageInsert),
			"source"_a,
			"type"_a,
			"id"_a,
			"severity"_a,
			"message"_a,
			"Inserts an application-defined GL debug message with an explicit source."
		)
		.def("messageInsert", py::overload_cast<
			osgx::debug::Type,
			GLuint,
			osgx::debug::Severity,
			const std::string&
		>(&osgx::debug::messageInsert),
			"type"_a,
			"id"_a,
			"severity"_a,
			"message"_a,
			"Inserts an application-defined GL debug message (source defaults to APPLICATION)."
		)
		.def(
			"pushGroup",
			py::overload_cast<osgx::debug::Source, GLuint, const std::string&>(&osgx::debug::pushGroup),
			"Pushes a named GL_KHR_debug group with an explicit source; pair with popGroup()."
		)
		.def(
			"pushGroup",
			py::overload_cast<GLuint, const std::string&>(&osgx::debug::pushGroup),
			"Pushes a named GL_KHR_debug group (source defaults to APPLICATION); pair with popGroup()."
		)
		.def("popGroup", &osgx::debug::popGroup, "Pops the most recently pushed GL_KHR_debug group.")
	;

	py::class_<osgx::debug::Scoped>(
		m_debug,
		"Scoped",
		"RAII/context-manager debug group: pushes on entry, pops on exit. Use as a `with` block "
		"around code whose begin and end live in the same scope; for begin/end split across "
		"separate draw callbacks, use AnnotationGroup instead."
	)
		.def(
			py::init<GLuint, std::string_view, osgx::debug::Source, bool>(),
			py::arg("id"),
			py::arg("message"),
			py::arg("source")=osgx::debug::Source::APPLICATION,
			py::arg("measureTime")=false,
			"Pushes the debug group immediately. If `measureTime` is True, a PERFORMANCE message "
			"reporting the elapsed wall time is inserted when the group is popped."
		)
		.def("__enter__", [](osgx::debug::Scoped& self) -> osgx::debug::Scoped& {
			return self;
		}, "Returns self; the group is already pushed by __init__.")
		.def("__exit__", [](
			osgx::debug::Scoped& self,
			py::object exc_type,
			py::object exc_value,
			py::object traceback
		) {
			return false; // don't suppress Python exceptions
		}, "exc_type"_a, "exc_value"_a, "traceback"_a, "Pops the debug group; never suppresses a raised exception.")
	;
}

}
