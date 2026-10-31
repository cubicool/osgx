#pragma once

#include "osgx/debug/Annotation.hpp"

OSGX_DISABLE_WARNINGS

#include <osgGA/GUIEventHandler>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <atomic>

namespace osgx::debug {

// An osgViewer::Viewer subclass for apitrace-friendly debugging. Press 'n' to
// advance one frame; the viewer idles between keystrokes so apitrace captures
// never flood with unintended frames.
//
// On first render, installs AnnotationBeginCallback / AnnotationEndCallback on
// the camera so every rendered frame appears in apitrace as:
//
// Frame { [all draw calls for this frame] }
//
// Any other object with its own RTT camera (e.g. osgSlug::Atlas) can install
// its own annotation callbacks and they will nest naturally inside this group.
class FrameByFrameViewer: public osgViewer::Viewer {
public:
	class EventHandler: public osgGA::GUIEventHandler {
	public:
		bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;
	};

	FrameByFrameViewer():
	_renderKeyHandler(new EventHandler()) {
		addEventHandler(_renderKeyHandler);
	}

	void requestRender() {
		_render.store(true, std::memory_order_release);
	}

	int run() override;

private:
	void _ensureInitialized();
	void _installAnnotationCallbacks();

	static constexpr unsigned int _POLL_INTERVAL_US = 100000;

	osg::ref_ptr<EventHandler> _renderKeyHandler;

	std::atomic<bool> _render{true};
	bool _firstFrame = true;
	bool _annotationCallbacksInstalled = false;
	osg::Timer_t _startTick = osg::Timer::instance()->tick();
	unsigned int _count = 0;
};

}
