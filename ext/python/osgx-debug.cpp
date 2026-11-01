#include "osgx-python.hpp"
#include "osgx/Debug.hpp"

#include <osgViewer/ViewerBase>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace osgx_python {

namespace {

// Python's AnnotationScope: osgx::debug::AnnotationScope pushes in its constructor and pops in
// its destructor, which Python does not tie to a `with` block, so the group is opened and closed
// by __enter__ and __exit__ instead.
struct AnnotationScope {
	AnnotationScope(GLuint id, std::string_view message, osgx::debug::Source source):
	group(id, message, source) {}

	osgx::debug::AnnotationGroup group;
};

// The Location of a Python trace scope named `name` (with `phase`, or none), one per name and
// phase, never freed: trace records point at their Location for as long as they are read.
const osgx::debug::Location& pythonLocation(const std::string& name, std::optional<osgx::debug::Phase> phase) {
	struct Entry {
		Entry(const std::string& n, std::optional<osgx::debug::Phase> p):
		name(n),
		location(p ? osgx::debug::Location(name.c_str(), *p) : osgx::debug::Location(name.c_str())) {
			location.file = "<python>";
			location.line = 0;
		}

		std::string name;
		osgx::debug::Location location;
	};

	struct Registry {
		std::mutex mutex;
		std::map<std::pair<std::string, int>, std::unique_ptr<Entry>> entries;
	};

	static auto* registry = new Registry();

	std::lock_guard<std::mutex> lock(registry->mutex);

	auto& entry = registry->entries[{name, phase ? static_cast<int>(*phase) : -1}];

	if(!entry) entry = std::make_unique<Entry>(name, phase);

	return entry->location;
}

// Python's TraceScope: osgx::debug::TraceScope records ENTER in its constructor and LEAVE in its
// destructor, so it is constructed by __enter__ and destroyed by __exit__.
struct TraceScope {
	TraceScope(const std::string& name, std::optional<osgx::debug::Phase> phase):
	location(pythonLocation(name, phase)) {}

	const osgx::debug::Location& location;
	std::optional<osgx::debug::TraceScope> scope;
};

}

void bind_debug(py::module_& m_debug) {
	py::enum_<osgx::debug::ClockCalibration>(
		m_debug,
		"ClockCalibration",
		"How ShaderClockDrawable obtains its shader-clock rate."
	)
		.value("DYNAMIC", osgx::debug::ClockCalibration::DYNAMIC)
		.value("STATIC", osgx::debug::ClockCalibration::STATIC)
		.export_values()
	;

	py::class_<osgx::debug::ShaderClockDrawable, osg::Drawable, osg::ref_ptr<osgx::debug::ShaderClockDrawable>>(
		m_debug,
		"ShaderClockDrawable",
		"A GL_ARB_shader_clock drawable that adds a calibrated GPU delay during traversal."
	)
		.def(py::init<osgx::debug::ClockCalibration>(), "mode"_a=osgx::debug::ClockCalibration::DYNAMIC)
		.def_property("duration", &osgx::debug::ShaderClockDrawable::getDuration, &osgx::debug::ShaderClockDrawable::setDuration, "Requested GPU delay in milliseconds.")
		.def_property_readonly("calibrationMode", &osgx::debug::ShaderClockDrawable::getCalibrationMode, "The configured ClockCalibration mode.")
		.def_property(
			"staticTicksPerMillisecond",
			&osgx::debug::ShaderClockDrawable::getStaticTicksPerMillisecond,
			&osgx::debug::ShaderClockDrawable::setTicksPerMillisecond,
			"Caller-provided shader-clock rate used in STATIC mode."
		)
		.def("getTicksPerMillisecond", &osgx::debug::ShaderClockDrawable::getTicksPerMillisecond, "contextID"_a=0)
		.def("recalibrate", &osgx::debug::ShaderClockDrawable::recalibrate)
		.def_property(
			"recalibrationInterval",
			&osgx::debug::ShaderClockDrawable::getRecalibrationInterval,
			&osgx::debug::ShaderClockDrawable::setRecalibrationInterval,
			"Elapsed seconds between DYNAMIC recalibrations; zero disables it."
		)
		.def_property(
			"recalibrationFrameInterval",
			&osgx::debug::ShaderClockDrawable::getRecalibrationFrameInterval,
			&osgx::debug::ShaderClockDrawable::setRecalibrationFrameInterval,
			"Rendered frames between DYNAMIC recalibrations; zero disables it."
		)
		.def("isSupported", &osgx::debug::ShaderClockDrawable::isSupported, "contextID"_a=0)
	;

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
			std::string_view
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
			std::string_view
		>(&osgx::debug::messageInsert),
			"type"_a,
			"id"_a,
			"severity"_a,
			"message"_a,
			"Inserts an application-defined GL debug message (source defaults to APPLICATION)."
		)
		.def(
			"pushGroup",
			py::overload_cast<osgx::debug::Source, GLuint, std::string_view>(&osgx::debug::pushGroup),
			"Pushes a named GL_KHR_debug group with an explicit source; pair with popGroup()."
		)
		.def(
			"pushGroup",
			py::overload_cast<GLuint, std::string_view>(&osgx::debug::pushGroup),
			"Pushes a named GL_KHR_debug group (source defaults to APPLICATION); pair with popGroup()."
		)
		.def("popGroup", &osgx::debug::popGroup, "Pops the most recently pushed GL_KHR_debug group.")
	;

	py::enum_<osgx::debug::Phase>(
		m_debug,
		"Phase",
		"The OSG traversal a trace record belongs to (GPU time is recorded on DRAW records)."
	)
		.value("EVENT", osgx::debug::Phase::EVENT)
		.value("UPDATE", osgx::debug::Phase::UPDATE)
		.value("CULL", osgx::debug::Phase::CULL)
		.value("DRAW", osgx::debug::Phase::DRAW)
	;

	py::class_<osgx::debug::TraceState>(
		m_debug,
		"TraceState",
		"Where the calling thread is, as its trace buffer knows it (threadTraceState())."
	)
		.def_readonly(
			"frame",
			&osgx::debug::TraceState::frame,
			"The frame of the thread's latest trace record; None before any."
		)
		.def_readonly(
			"phase",
			&osgx::debug::TraceState::phase,
			"The phase of the thread's innermost open span, else of its latest record; None before "
			"any. Trace scopes without a phase take it."
		)
		.def_readonly("depth", &osgx::debug::TraceState::depth, "The number of open spans.")
	;

	m_debug.def(
		"threadTraceState",
		[]() { return osgx::debug::threadTraceBuffer().state(); },
		"The calling thread's TraceState."
	);

	py::class_<TraceScope>(
		m_debug,
		"TraceScope",
		"Context manager that records a span named `name` in the calling thread's trace buffer for "
		"the duration of a `with` block: it shows up in the profiler's report and waterfall, nested "
		"under whatever span is open (an update callback, a camera's drawing, an annotation group). "
		"Without `phase` it takes the open span's phase (threadTraceState()); with none (nothing "
		"recorded on the thread yet) the span is not recorded. Without `phase`, inside an open DRAW "
		"span (a camera's drawing, a drawable's draw, an annotation) it is also a GL_KHR_debug "
		"group named `name`, visible in apitrace/RenderDoc/Nsight. Scopes with the same name and "
		"phase share one trace Location."
	)
		.def(
			py::init<const std::string&, std::optional<osgx::debug::Phase>>(),
			"name"_a,
			"phase"_a=py::none(),
			"Stores the scope; nothing is recorded until the `with` block is entered."
		)
		.def("__enter__", [](TraceScope& self) -> TraceScope& {
			self.scope.reset();
			self.scope.emplace(self.location);

			return self;
		}, "Records the span's start in the calling thread's buffer (the thread's latest frame); returns self.")
		.def("__exit__", [](
			TraceScope& self,
			py::object exc_type,
			py::object exc_value,
			py::object traceback
		) {
			self.scope.reset();

			return false;
		}, "exc_type"_a, "exc_value"_a, "traceback"_a, "Records the span's end; never suppresses a raised exception.")
	;

	py::enum_<osgx::debug::QueryMode>(
		m_debug,
		"QueryMode",
		"How the profiler reads its GPU timestamp queries: SYNC waits for the current frame's "
		"results (stalls until the GPU finishes it), ASYNC reads the previous frame's (no "
		"meaningful stall, one frame later)."
	)
		.value("SYNC", osgx::debug::QueryMode::SYNC)
		.value("ASYNC", osgx::debug::QueryMode::ASYNC)
	;

	py::enum_<osgx::debug::WaterfallGpu>(
		m_debug,
		"WaterfallGpu",
		"How a waterfall shows a node's GPU time: OFF not at all (GPU ms still listed), COST from "
		"the node's CPU start with the GPU time as its length (how much GPU work it adds), "
		"TIMELINE from its first draw's GPU start to its last draw's GPU end (when the GPU ran it; "
		"approximate, mapped onto CPU time by a clock sample)."
	)
		.value("OFF", osgx::debug::WaterfallGpu::OFF)
		.value("COST", osgx::debug::WaterfallGpu::COST)
		.value("TIMELINE", osgx::debug::WaterfallGpu::TIMELINE)
	;

	py::class_<osgx::debug::TraceNode>(
		m_debug,
		"TraceNode",
		"One node of a profile tree (Profiler.profile): a phase root, a camera group, a scene "
		"node or drawable, an annotation or a trace scope. Times are moving averages in "
		"milliseconds per frame over the frames the node appeared in. A snapshot: it does not "
		"change as the profile does."
	)
		.def_readonly("phase", &osgx::debug::TraceNode::phase, "The Phase whose tree holds the node.")
		.def_readonly("name", &osgx::debug::TraceNode::name, "The object's name or the record's text; may be empty.")
		.def_readonly(
			"label",
			&osgx::debug::TraceNode::label,
			"What to display: the object's name, else 'ClassName@0x<address>' for scene nodes and "
			"cameras, else the trace Location's name."
		)
		.def_property_readonly("className", [](const osgx::debug::TraceNode& node) -> std::optional<std::string> {
			if(!node.className) return std::nullopt;

			return std::string(node.className);
		}, "The object's class name; None without an object.")
		.def_property_readonly("location", [](const osgx::debug::TraceNode& node) -> std::optional<std::string> {
			if(!node.location) return std::nullopt;

			return std::string(node.location->name);
		}, "The name of the trace Location that recorded the node; None for groups without one.")
		.def_readonly("thread", &osgx::debug::TraceNode::thread, "The index of the trace buffer (thread) that recorded the node.")
		.def_readonly("inclusiveMs", &osgx::debug::TraceNode::inclusiveMs, "CPU ms including the children.")
		.def_readonly("selfMs", &osgx::debug::TraceNode::selfMs, "CPU ms excluding the children.")
		.def_readonly("drawMs", &osgx::debug::TraceNode::drawMs, "CPU ms of the drawable's draws (DRAW nodes; joined into CULL nodes).")
		.def_readonly("gpuMs", &osgx::debug::TraceNode::gpuMs, "GPU ms of the drawable's draws (DRAW nodes; joined into CULL nodes).")
		.def_readonly("count", &osgx::debug::TraceNode::count, "Spans per frame merged into the node.")
		.def_readonly("draws", &osgx::debug::TraceNode::draws, "Draws per frame.")
		.def_readonly("startMs", &osgx::debug::TraceNode::startMs, "Ms from the frame's start to the node's first record in it.")
		.def_readonly(
			"gpuStartMs",
			&osgx::debug::TraceNode::gpuStartMs,
			"Ms from the frame's start to the first draw's GPU start (mapped onto CPU time); -1 "
			"without GPU times."
		)
		.def_readonly("culled", &osgx::debug::TraceNode::culled, "Whether the cull visitor rejected the node in its latest frame.")
		.def_readonly("drawOrder", &osgx::debug::TraceNode::drawOrder, "The submission index of its first draw; -1 if not drawn.")
		.def_readonly("frame", &osgx::debug::TraceNode::frame, "The latest frame the node appeared in.")
		.def_property_readonly("children", [](py::object self) {
			py::list children;

			for(const auto& child : self.cast<const osgx::debug::TraceNode&>().children) {
				children.append(py::cast(&child, py::return_value_policy::reference_internal, self));
			}

			return children;
		}, "The child nodes, in traversal/submission order (they keep this node alive).")
		.def("__repr__", [](const osgx::debug::TraceNode& node) {
			return "<osgx.debug.TraceNode " + node.label + ">";
		}, "'<osgx.debug.TraceNode label>'.")
	;

	py::class_<osgx::debug::TraceSpan>(
		m_debug,
		"TraceSpan",
		"Where a node ran within a frame: startMs and endMs from the frame's start, and its ms."
	)
		.def_readonly("startMs", &osgx::debug::TraceSpan::startMs, "Ms from the frame's start.")
		.def_readonly("endMs", &osgx::debug::TraceSpan::endMs, "Ms from the frame's start.")
		.def_readonly("ms", &osgx::debug::TraceSpan::ms, "The span's ms (for a GPU span, the total GPU time of its draws).")
	;

	m_debug.def(
		"cpuSpan",
		&osgx::debug::cpuSpan,
		"node"_a,
		"frame"_a,
		"The node's CPU TraceSpan in `frame` (its own spans, else its children's); None if it did "
		"not run."
	);

	m_debug.def(
		"gpuSpan",
		&osgx::debug::gpuSpan,
		"node"_a,
		"frame"_a,
		"The GPU TraceSpan of the draws of the node and its descendants in `frame`: first GPU "
		"start to last GPU end, ms their total GPU time; None without GPU times."
	);

	py::class_<osgx::debug::FrameLoad>(
		m_debug,
		"FrameLoad",
		"The load of a frame in ms (frameLoad()): cpuMs (the busiest thread's CPU busy time, waits "
		"such as the swap and readback excluded), gpuMs (the draws' GPU time), where the waterfall "
		"bars end per WaterfallGpu, and where the GPU work starts. Compare busy times with a "
		"budget, not where spans end (with vsync a frame always ends near the refresh period)."
	)
		.def_readonly("cpuMs", &osgx::debug::FrameLoad::cpuMs, "CPU busy time of the busiest thread.")
		.def_readonly("gpuMs", &osgx::debug::FrameLoad::gpuMs, "GPU busy time (the draws' GPU time).")
		.def_readonly("cpuEndMs", &osgx::debug::FrameLoad::cpuEndMs, "Where the CPU spans end.")
		.def_readonly("costEndMs", &osgx::debug::FrameLoad::costEndMs, "Where the bars end with WaterfallGpu.COST.")
		.def_readonly("timelineEndMs", &osgx::debug::FrameLoad::timelineEndMs, "Where the bars end with WaterfallGpu.TIMELINE.")
		.def_readonly("gpuStartMs", &osgx::debug::FrameLoad::gpuStartMs, "Where the GPU work starts; -1 without GPU times.")
		.def("endMs", &osgx::debug::FrameLoad::endMs, "gpu"_a, "cpuEndMs, costEndMs or timelineEndMs, for `gpu`.")
	;

	m_debug.def(
		"frameLoad",
		&osgx::debug::frameLoad,
		"profile"_a,
		"frame"_a,
		"The FrameLoad of `frame` in `profile` (Profiler.profile, the four phase trees)."
	);

	py::class_<osgx::debug::Profiler, osg::Referenced, osg::ref_ptr<osgx::debug::Profiler>>(
		m_debug,
		"Profiler",
		"Profiles a viewer by OSG phase (event, update, cull, draw + GPU) into a rolling tree of "
		"moving averages: attach() it once, after the views' scene data is set and before the "
		"viewer is realized. Keep a reference to it: the viewer's callbacks only observe it. Python "
		"code shows up in it through TraceScope."
	)
		.def(py::init<osgx::debug::QueryMode>(), "mode"_a=osgx::debug::QueryMode::SYNC, "Creates a profiler (not attached).")
		.def("attach", &osgx::debug::Profiler::attach, "viewer"_a, "Installs the profiler on every view of `viewer`.")
		.def(
			"refresh",
			&osgx::debug::Profiler::refresh,
			"Re-scans the views' scenes, so drawables added since attach() are profiled too."
		)
		.def_property(
			"mode",
			&osgx::debug::Profiler::getMode,
			&osgx::debug::Profiler::setMode,
			"The QueryMode of the GPU queries."
		)
		.def_property_readonly(
			"profile",
			&osgx::debug::Profiler::getProfile,
			"A snapshot of the four phase trees (EVENT, UPDATE, CULL, DRAW) as TraceNodes."
		)
		.def_property_readonly(
			"profileFrame",
			&osgx::debug::Profiler::getProfileFrame,
			"The newest frame in the profile; None before the first is folded."
		)
		.def("report", [](const osgx::debug::Profiler& self) {
			std::ostringstream out;

			self.report(out);

			return out.str();
		}, "The four phase trees as indented text (inclusive/self ms, spans, draw CPU/GPU, order).")
		.def("reportWaterfall", [](
			const osgx::debug::Profiler& self,
			double budgetMs,
			std::size_t width,
			std::size_t maxDepth,
			double minMs,
			osgx::debug::WaterfallGpu gpu
		) {
			std::ostringstream out;

			self.reportWaterfall(out, budgetMs, width, maxDepth, minMs, gpu);

			return out.str();
		},
			"budgetMs"_a=1000.0 / 60.0,
			"width"_a=64,
			"maxDepth"_a=3,
			"minMs"_a=0.05,
			"gpu"_a=osgx::debug::WaterfallGpu::COST,
			"The newest frame as a text waterfall: a row per node with its CPU and GPU ms and a bar "
			"where it ran within the frame, against the budget."
		)
	;

	py::class_<
		osgx::debug::AnnotationGroup,
		osg::Referenced,
		osg::ref_ptr<osgx::debug::AnnotationGroup>
	>(
		m_debug,
		"AnnotationGroup",
		"A debug group opened and closed by explicit begin()/end() calls, for a group whose begin "
		"and end run in separate draw callbacks (e.g. PRE_DRAW and POST_DRAW). Both calls must run "
		"on the GL thread with the context current."
	)
		.def(
			py::init<GLuint, std::string_view, osgx::debug::Source>(),
			"id"_a,
			"message"_a,
			"source"_a=osgx::debug::Source::APPLICATION,
			"Stores the group; nothing is pushed until begin()."
		)
		.def(
			"begin",
			&osgx::debug::AnnotationGroup::begin,
			"Pushes the group (and records it as a trace span); does nothing if it is already open."
		)
		.def(
			"end",
			&osgx::debug::AnnotationGroup::end,
			"Pops the group; does nothing if it is not open."
		)
	;

	py::class_<AnnotationScope>(
		m_debug,
		"AnnotationScope",
		"Context-manager debug group: pushes on entering a `with` block, pops on leaving it "
		"(exception or not). Use around code whose begin and end live in the same block, on the "
		"GL thread with the context current; for begin/end split across separate draw callbacks, "
		"use AnnotationGroup instead."
	)
		.def(
			py::init<GLuint, std::string_view, osgx::debug::Source>(),
			"id"_a,
			"message"_a,
			"source"_a=osgx::debug::Source::APPLICATION,
			"Stores the group; nothing is pushed until the `with` block is entered."
		)
		.def("__enter__", [](AnnotationScope& self) -> AnnotationScope& {
			self.group.begin();

			return self;
		}, "Pushes the debug group (and records it as a trace span); returns self.")
		.def("__exit__", [](
			AnnotationScope& self,
			py::object exc_type,
			py::object exc_value,
			py::object traceback
		) {
			self.group.end();

			return false;
		}, "exc_type"_a, "exc_value"_a, "traceback"_a, "Pops the debug group; never suppresses a raised exception.")
	;
}

}
