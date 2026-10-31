// vimrun! ./examples/osgx-profiling --frames 300

// A scene with known, code-controlled costs in each OSG phase, for checking profiler output
// against expected numbers:
//
// - UPDATE: node update callbacks that wait a fixed time, nested (parent over child).
// - CULL: a node cull callback and a drawable cull callback that wait a fixed time, plus one
//   object behind the camera that is always frustum culled.
// - DRAW (CPU): a drawable draw callback that waits a fixed time.
// - DRAW (GPU): ShaderClockDrawables that keep the GPU busy a fixed time (GL_ARB_shader_clock).
// - A pre-render RTT camera with its own GPU cost, sharing one drawable with the main camera.
//
// Waits spin on osg::Timer by default (attributed CPU work); --sleep makes them sleep instead
// (blocking, which a timeline shows as a gap). On exit it prints osgViewer's own per-phase stats,
// the osgx::debug profiler's per-drawable results, and a table of every configured cost next to
// the measurement that corresponds to it.
//
// Options:
//   --frames N        Render N frames, then print the report and exit (0: until the window
//                     closes; default 300 with --headless).
//   --scale F         Multiply every configured cost by F (default 1).
//   --sleep           Sleep instead of spinning for the CPU costs.
//   --async           Use QueryMode::ASYNC for the profiler's GPU queries.
//   --headless [egl|native]
//                     Render into an offscreen pbuffer (no window).
//   --SingleThreaded, --DrawThreadPerContext, --CullDrawThreadPerContext,
//   --CullThreadPerCameraDrawThreadPerContext
//                     osgViewer threading model (default SingleThreaded).

#include "osgx/Core.hpp"
#include "osgx/Debug.hpp"
#include "osgx/Headless.hpp"
#include "osgx/Library.hpp"
#include "osgx/Visitors.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osg/Stats>
#include <osg/Texture2D>
#include <osg/Timer>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Configured costs in milliseconds, before --scale.
struct Costs {
	double updateParent = 1.0;
	double updateChild = 2.0;
	double cullNode = 1.0;
	double cullDrawable = 0.5;
	double drawCPU = 1.0;
	double gpuSmall = 1.0;
	double gpuLarge = 2.0;
	double gpuRTT = 4.0;

	void scale(double factor) {
		for(double* cost : {
			&updateParent,
			&updateChild,
			&cullNode,
			&cullDrawable,
			&drawCPU,
			&gpuSmall,
			&gpuLarge,
			&gpuRTT
		}) *cost *= factor;
	}
};

bool sleepWait = false;

void waitFor(double milliseconds) {
	if(sleepWait) {
		std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(milliseconds));

		return;
	}

	auto* timer = osg::Timer::instance();
	const auto start = timer->tick();

	while(timer->delta_m(start, timer->tick()) < milliseconds) {}
}

// Waits, then traverses. Used as both an update and a cull callback.
class WaitCallback: public osg::NodeCallback {
public:
	explicit WaitCallback(double milliseconds):
	_ms(milliseconds) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		waitFor(_ms);
		traverse(node, nv);
	}

private:
	double _ms;
};

class WaitDrawableCullCallback: public osg::DrawableCullCallback {
public:
	explicit WaitDrawableCullCallback(double milliseconds):
	_ms(milliseconds) {}

	bool cull(osg::NodeVisitor*, osg::Drawable*, osg::RenderInfo*) const override {
		waitFor(_ms);

		return false;
	}

private:
	double _ms;
};

class WaitDrawCallback: public osg::Drawable::DrawCallback {
public:
	explicit WaitDrawCallback(double milliseconds):
	_ms(milliseconds) {}

	void drawImplementation(osg::RenderInfo& ri, const osg::Drawable* drawable) const override {
		waitFor(_ms);
		drawable->drawImplementation(ri);
	}

private:
	double _ms;
};

osg::Geode* makeBox(const std::string& name, const osg::Vec3& center, const osg::Vec4& color) {
	auto* box = new osg::ShapeDrawable(new osg::Box(center, 1.0f));
	auto* geode = new osg::Geode();

	box->setName(name);
	box->setColor(color);
	geode->setName(name + "_geode");
	geode->addDrawable(box);

	return geode;
}

osg::Geode* makeClock(
	const std::string& name,
	double milliseconds,
	std::vector<osg::ref_ptr<osgx::debug::ShaderClockDrawable>>& clocks
) {
	auto* clock = new osgx::debug::ShaderClockDrawable();
	auto* geode = new osg::Geode();

	clock->setName(name);
	clock->setDuration(milliseconds);

	geode->setName(name + "_geode");
	geode->addDrawable(clock);
	clocks.push_back(clock);

	return geode;
}

osg::Camera* makeRTTCamera(int size) {
	auto* texture = new osg::Texture2D();

	texture->setTextureSize(size, size);
	texture->setInternalFormat(GL_RGBA8);

	auto* camera = new osg::Camera();

	camera->setName("rtt");
	camera->setRenderOrder(osg::Camera::PRE_RENDER);
	camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
	camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
	camera->setViewport(0, 0, size, size);
	camera->setClearColor(osg::Vec4(0.2f, 0.1f, 0.1f, 1.0f));
	camera->setProjectionMatrixAsPerspective(30.0, 1.0, 1.0, 100.0);
	camera->setViewMatrixAsLookAt(osg::Vec3(0.0, -12.0, 4.0), osg::Vec3(), osg::Vec3(0.0, 0.0, 1.0));
	camera->attach(osg::Camera::COLOR_BUFFER, texture);

	return camera;
}

const char* threadingName(osgViewer::ViewerBase::ThreadingModel model) {
	switch(model) {
		case osgViewer::ViewerBase::SingleThreaded: return "SingleThreaded";
		case osgViewer::ViewerBase::CullDrawThreadPerContext: return "CullDrawThreadPerContext";
		case osgViewer::ViewerBase::DrawThreadPerContext: return "DrawThreadPerContext";
		case osgViewer::ViewerBase::CullThreadPerCameraDrawThreadPerContext:
			return "CullThreadPerCameraDrawThreadPerContext";
		default: return "AutomaticSelection";
	}
}

double ticksToMs(double ticks) {
	return ticks * osg::Timer::instance()->getSecondsPerTick() * 1000.0;
}

// Prints and returns (in milliseconds) an osg::Stats attribute averaged over [first, last].
std::optional<double> printStat(
	const osg::Stats* stats,
	unsigned int first,
	unsigned int last,
	const char* label,
	const std::string& attribute
) {
	double value = 0.0;
	std::optional<double> ms;

	std::printf("  %-28s", label);

	if(stats && stats->getAveragedAttribute(first, last, attribute, value)) {
		ms = value * 1000.0;

		std::printf("%9.3f ms", *ms);
	}

	else std::printf("%12s", "-");

	std::cout << std::endl;

	return ms;
}

}

int main(int argc, char** argv) {
	osg::ArgumentParser arguments(&argc, argv);

	auto headless = osgx::headless::readArguments(arguments);

	Costs costs;
	double scale = 1.0;
	unsigned int frames = 0;

	arguments.read("--scale", scale);
	arguments.read("--frames", frames);
	sleepWait = arguments.read("--sleep");

	const bool async = arguments.read("--async");
	const bool threadingRequested =
		arguments.find("--SingleThreaded") >= 0 ||
		arguments.find("--DrawThreadPerContext") >= 0 ||
		arguments.find("--CullDrawThreadPerContext") >= 0 ||
		arguments.find("--CullThreadPerCameraDrawThreadPerContext") >= 0
	;

	if(headless.enabled && !frames) frames = 300;

	costs.scale(scale);

	auto lib = osgx::initialize();

	// The scene, every node named:
	//
	// root
	//   update (update: updateParent)
	//     update_child (update: updateChild)
	//       update_box
	//   cull (cull: cullNode)
	//     cull_box (drawable cull: cullDrawable)
	//   draw_box (draw: drawCPU)
	//   gpu_small, gpu_large (ShaderClockDrawables)
	//   culled (behind the camera)
	//     culled_box
	//   shared_box (also under rtt)
	//   rtt (PRE_RENDER camera)
	//     gpu_rtt (ShaderClockDrawable)
	//     shared_box
	std::vector<osg::ref_ptr<osgx::debug::ShaderClockDrawable>> clocks;

	auto root = osgx::make_nref<osg::Group>("root");
	auto update = osgx::make_nref<osg::Group>("update");
	auto updateChild = osgx::make_nref<osg::Group>("update_child");

	update->setUpdateCallback(new WaitCallback(costs.updateParent));
	updateChild->setUpdateCallback(new WaitCallback(costs.updateChild));
	updateChild->addChild(makeBox("update_box", osg::Vec3(-3.0f, 0.0f, 0.0f), osg::Vec4(0.9f, 0.3f, 0.3f, 1.0f)));
	update->addChild(updateChild);

	auto cull = osgx::make_nref<osg::Group>("cull");
	auto* cullBox = makeBox("cull_box", osg::Vec3(-1.0f, 0.0f, 0.0f), osg::Vec4(0.3f, 0.9f, 0.3f, 1.0f));

	cull->setCullCallback(new WaitCallback(costs.cullNode));
	cullBox->getDrawable(0)->setCullCallback(new WaitDrawableCullCallback(costs.cullDrawable));
	cull->addChild(cullBox);

	auto* drawBox = makeBox("draw_box", osg::Vec3(1.0f, 0.0f, 0.0f), osg::Vec4(0.3f, 0.3f, 0.9f, 1.0f));

	drawBox->getDrawable(0)->setDrawCallback(new WaitDrawCallback(costs.drawCPU));

	auto culled = osgx::make_nref<osg::MatrixTransform>("culled");

	culled->setMatrix(osg::Matrix::translate(0.0, -40.0, 0.0));
	culled->addChild(makeBox("culled_box", osg::Vec3(), osg::Vec4(0.5f, 0.5f, 0.5f, 1.0f)));

	auto* sharedBox = makeBox("shared_box", osg::Vec3(3.0f, 0.0f, 0.0f), osg::Vec4(0.9f, 0.9f, 0.3f, 1.0f));
	auto* rtt = makeRTTCamera(256);

	rtt->addChild(makeClock("gpu_rtt", costs.gpuRTT, clocks));
	rtt->addChild(sharedBox);

	root->addChild(update);
	root->addChild(cull);
	root->addChild(drawBox);
	root->addChild(makeClock("gpu_small", costs.gpuSmall, clocks));
	root->addChild(makeClock("gpu_large", costs.gpuLarge, clocks));
	root->addChild(culled);
	root->addChild(sharedBox);
	root->addChild(rtt);

	osgViewer::Viewer viewer(arguments);

	if(!threadingRequested) viewer.setThreadingModel(osgViewer::ViewerBase::SingleThreaded);

	auto* camera = viewer.getCamera();

	camera->setName("main");
	camera->setClearColor(osg::Vec4(0.1f, 0.1f, 0.15f, 1.0f));

	// Fixed view, no manipulator: the "culled" subtree stays behind the camera.
	camera->setViewMatrixAsLookAt(osg::Vec3(0.0, -14.0, 5.0), osg::Vec3(), osg::Vec3(0.0, 0.0, 1.0));

	if(headless.enabled) {
		osgx::headless::Context context;

		try {
			context = osgx::headless::createContext(
				headless.width,
				headless.height,
				std::nullopt,
				headless.backend
			);
		}

		catch(const std::exception& exception) {
			std::cerr << "headless: " << exception.what() << std::endl;

			return 1;
		}

		camera->setGraphicsContext(context.graphicsContext);
		camera->setViewport(new osg::Viewport(0, 0, headless.width, headless.height));
		camera->setProjectionMatrixAsPerspective(
			30.0,
			static_cast<double>(headless.width) / static_cast<double>(headless.height),
			1.0,
			100.0
		);
	}

	else if(!camera->getGraphicsContext()) viewer.setUpViewInWindow(80, 80, 1024, 768);

	viewer.setSceneData(root);

	// Per-drawable GPU/CPU timing and cull state, drained once per frame by the main camera's
	// FINAL_DRAW callback.
	auto profiler = osgx::make_ref<osgx::debug::Profiler>(
		async ? osgx::debug::QueryMode::ASYNC : osgx::debug::QueryMode::SYNC
	);

	profiler->attach(viewer);

	// Room for a full averaging window; osg::Stats keeps 25 frames by default.
	constexpr unsigned int STATS_FRAMES = 120;

	viewer.setViewerStats(new osg::Stats("Viewer", STATS_FRAMES));
	viewer.getViewerStats()->collectStats("frame_rate", true);
	viewer.getViewerStats()->collectStats("event", true);
	viewer.getViewerStats()->collectStats("update", true);
	camera->setStats(new osg::Stats("Camera", STATS_FRAMES));
	camera->getStats()->collectStats("rendering", true);
	camera->getStats()->collectStats("gpu", true);

	viewer.realize();

	// The GPU clock rate rises under sustained load (boost), so the ShaderClockDrawables'
	// first-draw calibration runs short afterwards. Calibrating again once the rate has settled
	// keeps calibration work out of later frames, which the reports average.
	constexpr unsigned int CALIBRATION_FRAME = 60;

	for(unsigned int frame = 0; !viewer.done() && (!frames || frame < frames); frame++) {
		if(frame == CALIBRATION_FRAME) {
			for(auto& clock : clocks) clock->recalibrate();
		}

		viewer.frame();
	}

	// Stop threads so the stats and profiler results are no longer being written.
	viewer.stopThreading();

	std::cout << std::endl << "osgx-profiling: threading model " << threadingName(viewer.getThreadingModel())
		<< ", " << (sleepWait ? "sleep" : "spin") << " waits, "
		<< (async ? "ASYNC" : "SYNC") << " queries, scale " << scale << std::endl;

	// GPU stats arrive a few frames late; leave the newest frames out of the window.
	const auto* viewerStats = viewer.getViewerStats();
	const auto* cameraStats = camera->getStats();
	const unsigned int last = viewerStats->getLatestFrameNumber() > 3
		? viewerStats->getLatestFrameNumber() - 3
		: 0u
	;
	const unsigned int first = std::max(
		viewerStats->getEarliestFrameNumber(),
		last > 100 ? last - 100 : 0u
	);

	std::cout << std::endl << "osgViewer stats (frames " << first << "-" << last << "):" << std::endl;

	printStat(viewerStats, first, last, "Frame duration", "Frame duration");
	printStat(viewerStats, first, last, "Event traversal", "Event traversal time taken");

	const auto updateMs = printStat(viewerStats, first, last, "Update traversal", "Update traversal time taken");
	const auto cullMs = printStat(cameraStats, first, last, "Cull traversal (main)", "Cull traversal time taken");

	printStat(cameraStats, first, last, "Draw traversal (main)", "Draw traversal time taken");

	const auto gpuDrawMs = printStat(cameraStats, first, last, "GPU draw (main)", "GPU draw time taken");

	// osgx::debug profiler: GPU time, CPU time (between the drawable's trace records) and cull state
	// per camera-qualified drawable path.
	const auto contextID = camera->getGraphicsContext()->getState()->getContextID();
	const auto stats = osgx::debug::profilerStats(contextID);
	const std::map<std::string, osgx::debug::detail::FrameAccumulator::PathStats> sorted(
		stats.begin(),
		stats.end()
	);

	std::cout << std::endl << "osgx::debug profiler, GPU and CPU (rolling averages) and cull state:"
		<< std::endl;

	for(const auto& [path, ps] : sorted) {
		const char* cullState =
			ps.cullState == osgx::debug::CullState::VISIBLE ? "visible" :
			ps.cullState == osgx::debug::CullState::CULLED ? "culled" :
			"unknown"
		;

		std::printf(
			"  %-48s%9.3f ms GPU%9.3f ms CPU  %-8s(%zu samples)",
			path.c_str(),
			static_cast<double>(ps.gpuBuffer.average()) / 1000000.0,
			static_cast<double>(ps.cpuBuffer.average()) / 1000000.0,
			cullState,
			ps.gpuBuffer.size()
		);

		std::cout << std::endl;
	}

	// ProfilerCallback's own CPU time around each drawable's draw; one callback per drawable, so a
	// drawable drawn by both cameras (shared_box) mixes both into one average.
	std::cout << std::endl << "osgx::debug ProfilerCallback, CPU draw (rolling average):" << std::endl;

	std::map<std::string, double> cpuDrawMs;

	osgx::LambdaVisitor<osg::Node> cpuVisitor([&cpuDrawMs](osg::Node& node) {
		auto* drawable = node.asDrawable();

		if(!drawable) return;

		auto* callback = dynamic_cast<osgx::debug::ProfilerCallback<>*>(drawable->getDrawCallback());

		if(!callback) return;

		const double ms = ticksToMs(static_cast<double>(callback->cpuBuffer().average()));

		cpuDrawMs[drawable->getName()] = ms;

		std::printf("  %-48s%9.3f ms", callback->getProfilerName().c_str(), ms);

		std::cout << std::endl;
	});

	root->accept(cpuVisitor);

	// The profiler's stats for the drawable named `name` as drawn by the camera named `cameraName`.
	auto findStats = [&sorted](
		const std::string& cameraName,
		const std::string& name
	) -> const osgx::debug::detail::FrameAccumulator::PathStats* {
		const auto suffix = "/" + name;

		for(const auto& [path, ps] : sorted) {
			if(path.starts_with(cameraName + "/") && path.ends_with(suffix) && ps.gpuBuffer.size()) return &ps;
		}

		return nullptr;
	};

	auto gpuMs = [&findStats](const std::string& cameraName, const std::string& name) -> std::optional<double> {
		const auto* ps = findStats(cameraName, name);

		if(!ps) return std::nullopt;

		return static_cast<double>(ps->gpuBuffer.average()) / 1000000.0;
	};

	auto traceCPUMs = [&findStats](const std::string& cameraName, const std::string& name) -> std::optional<double> {
		const auto* ps = findStats(cameraName, name);

		if(!ps) return std::nullopt;

		return static_cast<double>(ps->cpuBuffer.average()) / 1000000.0;
	};

	auto cpuMs = [&cpuDrawMs](const std::string& name) -> std::optional<double> {
		auto it = cpuDrawMs.find(name);

		if(it == cpuDrawMs.end()) return std::nullopt;

		return it->second;
	};

	const auto gpuSmallMs = gpuMs("main", "gpu_small");
	const auto gpuLargeMs = gpuMs("main", "gpu_large");
	const auto gpuRTTMs = gpuMs("rtt", "gpu_rtt");

	std::optional<double> gpuSumMs;

	if(gpuSmallMs && gpuLargeMs && gpuRTTMs) gpuSumMs = *gpuSmallMs + *gpuLargeMs + *gpuRTTMs;

	struct Row {
		const char* label;
		double expected;
		std::optional<double> measured;
		const char* source;
	};

	// Totals include overhead beyond the configured costs, so they read slightly high.
	const std::vector<Row> rows = {
		{"UPDATE update (self)", costs.updateParent, std::nullopt, "not measured"},
		{"UPDATE update_child", costs.updateChild, std::nullopt, "not measured"},
		{"UPDATE total", costs.updateParent + costs.updateChild, updateMs, "osgViewer update"},
		{"CULL   cull (node)", costs.cullNode, std::nullopt, "not measured"},
		{"CULL   cull_box (drawable)", costs.cullDrawable, std::nullopt, "not measured"},
		{"CULL   total", costs.cullNode + costs.cullDrawable, cullMs, "osgViewer cull (main)"},
		{"DRAW   draw_box (CPU)", costs.drawCPU, traceCPUMs("main", "draw_box"), "profiler trace CPU (main)"},
		{"DRAW   draw_box (CPU)", costs.drawCPU, cpuMs("draw_box"), "ProfilerCallback CPU"},
		{"GPU    gpu_small", costs.gpuSmall, gpuSmallMs, "profiler GPU (main)"},
		{"GPU    gpu_large", costs.gpuLarge, gpuLargeMs, "profiler GPU (main)"},
		{"GPU    gpu_rtt", costs.gpuRTT, gpuRTTMs, "profiler GPU (rtt)"},
		{"GPU    clocks (sum)", costs.gpuSmall + costs.gpuLarge + costs.gpuRTT, gpuSumMs, "profiler GPU"},
		{"GPU    total", costs.gpuSmall + costs.gpuLarge + costs.gpuRTT, gpuDrawMs, "osgViewer GPU (main)"}
	};

	std::cout << std::endl << "Expected vs. measured:" << std::endl;
	std::printf("  %-28s%12s%12s%9s  %s", "", "expected", "measured", "ratio", "source");
	std::cout << std::endl;

	for(const auto& row : rows) {
		std::printf("  %-28s%9.3f ms", row.label, row.expected);

		if(row.measured) {
			std::printf("%9.3f ms%9.2f", *row.measured, *row.measured / row.expected);
		}

		else std::printf("%12s%9s", "-", "-");

		std::printf("  %s", row.source);
		std::cout << std::endl;
	}

	// Each thread's trace buffer: DRAW records of the newest frame whose GPU results have certainly
	// been written back (two frames back covers ASYNC's one-frame lag).
	std::cout << std::endl << "osgx::debug trace buffers:" << std::endl;

	for(const auto& buffer : osgx::debug::traceBuffers()) {
		const auto frameNumbers = buffer->frames();

		std::cout << "  [" << buffer->index() << "] " << buffer->getLabel() << ": " << buffer->head()
			<< " records, capacity " << buffer->capacity();

		if(frameNumbers.size() < 3) {
			std::cout << std::endl;

			continue;
		}

		const auto frame = frameNumbers[frameNumbers.size() - 3];
		const auto range = buffer->frameRange(frame);
		std::size_t draws = 0;
		std::size_t withGPU = 0;

		for(auto sequence = range ? range->first : 0; range && sequence < range->end; sequence++) {
			osgx::debug::TraceRecord record;

			if(
				!buffer->read(sequence, record) ||
				record.kind != osgx::debug::TraceRecord::Kind::ENTER ||
				record.location->phase != osgx::debug::Phase::DRAW
			) continue;

			draws++;

			if(record.gpuEnd > record.gpuBegin) withGPU++;
		}

		std::cout << "; frame " << frame << ": " << draws << " DRAW records, " << withGPU
			<< " with GPU times" << std::endl;
	}

	for(const auto& clock : clocks) {
		if(!clock->isSupported(contextID)) {
			std::cout << std::endl << "WARNING: GL_ARB_shader_clock unsupported; "
				<< clock->getName() << " costs nothing" << std::endl;
		}
	}

	return 0;
}
