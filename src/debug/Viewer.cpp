#include "osgx/debug/Viewer.hpp"

OSGX_DISABLE_WARNINGS

#include <OpenThreads/Thread>

OSGX_ENABLE_WARNINGS

namespace osgx::debug {

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
