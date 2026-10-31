#pragma once

#include "osgx/debug/Annotation.hpp"
#include "osgx/debug/Profiler.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osgGA/GUIEventHandler>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <atomic>
#include <ostream>

namespace osgx::debug {

// An osgViewer::Viewer with its own Profiler: attached in realize() (before the viewer's threads
// start) and refreshed by setSceneData(). Its event and update traversals are EVENT and UPDATE
// spans of their own ("event traversal", "update traversal"), the roots of those trees in the
// profile: their self time is the work outside the scene visitors (event handlers, the camera
// manipulator, database pager merges, update operations).
class ProfilingViewer: public osgViewer::Viewer {
public:
	explicit ProfilingViewer(QueryMode mode=QueryMode::SYNC);
	explicit ProfilingViewer(osg::ArgumentParser& arguments, QueryMode mode=QueryMode::SYNC);
	~ProfilingViewer() override;

	Profiler* getProfiler();
	const Profiler* getProfiler() const;

	void realize() override;
	void setSceneData(osg::Node* node) override;
	void eventTraversal() override;
	void updateTraversal() override;

private:
	unsigned int _frame() const;

	osg::ref_ptr<Profiler> _profiler;
	bool _attached = false;
};

// A ProfilingViewer for apitrace-friendly debugging. Press 'n' to render one frame; between
// keystrokes the viewer only processes events (frame numbers count rendered frames) and idles, so
// apitrace captures never flood with unintended frames.
//
// On first render, installs AnnotationBeginCallback / AnnotationEndCallback on
// the camera so every rendered frame appears in apitrace as:
//
// Frame { [all draw calls for this frame] }
//
// Any other object with its own RTT camera (e.g. osgSlug::Atlas) can install
// its own annotation callbacks and they will nest naturally inside this group.
//
// setReportStream() writes the profile after each rendered frame. Frames are folded into the
// profile one frame late (see Profiler), so it shows the frame rendered before.
class FrameByFrameViewer: public ProfilingViewer {
public:
	class EventHandler: public osgGA::GUIEventHandler {
	public:
		bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;
	};

	FrameByFrameViewer();
	~FrameByFrameViewer() override;

	void requestRender();

	// Null (the default) writes nothing.
	void setReportStream(std::ostream* out);

	int run() override;

private:
	void _installAnnotationCallbacks();

	static constexpr unsigned int _POLL_INTERVAL_US = 100000;

	osg::ref_ptr<EventHandler> _renderKeyHandler;

	std::atomic<bool> _render{true};
	bool _annotationCallbacksInstalled = false;
	std::ostream* _report = nullptr;
};

}
