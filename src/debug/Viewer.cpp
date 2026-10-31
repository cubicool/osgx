#include "osgx/debug/Viewer.hpp"

OSGX_DISABLE_WARNINGS

#include <OpenThreads/Thread>

OSGX_ENABLE_WARNINGS

namespace osgx::debug {

namespace {

constexpr Location EVENT_TRAVERSAL("event traversal", Phase::EVENT);
constexpr Location UPDATE_TRAVERSAL("update traversal", Phase::UPDATE);

}

ProfilingViewer::ProfilingViewer(QueryMode mode):
_profiler(new Profiler(mode)) {}

ProfilingViewer::ProfilingViewer(osg::ArgumentParser& arguments, QueryMode mode):
osgViewer::Viewer(arguments),
_profiler(new Profiler(mode)) {}

ProfilingViewer::~ProfilingViewer() = default;

Profiler* ProfilingViewer::getProfiler() {
	return _profiler.get();
}

const Profiler* ProfilingViewer::getProfiler() const {
	return _profiler.get();
}

void ProfilingViewer::realize() {
	if(!_attached) {
		_profiler->attach(*this);
		_attached = true;
	}

	osgViewer::Viewer::realize();
}

void ProfilingViewer::setSceneData(osg::Node* node) {
	osgViewer::Viewer::setSceneData(node);

	if(_attached) _profiler->refresh();
}

unsigned int ProfilingViewer::_frame() const {
	return getFrameStamp() ? getFrameStamp()->getFrameNumber() : 0u;
}

void ProfilingViewer::eventTraversal() {
	TraceScope scope(EVENT_TRAVERSAL, this, _frame());

	osgViewer::Viewer::eventTraversal();
}

void ProfilingViewer::updateTraversal() {
	TraceScope scope(UPDATE_TRAVERSAL, this, _frame());

	osgViewer::Viewer::updateTraversal();
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

FrameByFrameViewer::FrameByFrameViewer():
_renderKeyHandler(new EventHandler()) {
	addEventHandler(_renderKeyHandler);
}

FrameByFrameViewer::~FrameByFrameViewer() = default;

void FrameByFrameViewer::requestRender() {
	_render.store(true, std::memory_order_release);
}

void FrameByFrameViewer::setReportStream(std::ostream* out) {
	_report = out;
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
		_installAnnotationCallbacks();

		if(_render.exchange(false, std::memory_order_acq_rel)) {
			// frame() realizes the viewer on its first call, then runs advance(), eventTraversal(),
			// updateTraversal() and renderingTraversals(). renderingTraversals() does:
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
			frame();

			if(_report) getProfiler()->report(*_report);
		}

		// Between frames only events are processed (the 'n' key, closing the window); their
		// trace records belong to the last rendered frame.
		else eventTraversal();

		OpenThreads::Thread::microSleep(_POLL_INTERVAL_US);
	}

	return 0;
}

}
