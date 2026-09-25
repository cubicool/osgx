// vimrun! ./examples/osgx-platform
//
// Demonstrates osgx::platform (osgx/Linux.hpp, osgx/GraphicsWindowEGL.hpp,
// osgx/GraphicsWindowGBM.hpp). Inspired by OpenSceneGraph.py's examples/pyosg-linux.py, which this
// example replaces the C++ side of.
//
// Default: an ordinary GLX window. Prints the real XRandR monitor layout (listMonitors()), moves
// the window 40px in from the primary monitor's origin (moveWindow()), and pins it above other
// windows once realized (alwaysOnTop()).
//
// --egl            drive the window with EGL instead of GLX (requires OSGX_WITH_EGL; still an
//                   ordinary X11 window under the hood, so moveWindow()/alwaysOnTop() would work
//                   the same way, but this demo keeps those X11-window-manager calls scoped to the
//                   default GLX path for clarity)
// --gbm            direct DRM/KMS scanout, no X11 at all (requires OSGX_WITH_GBM; needs exclusive
//                   DRM master access, so run from a bare TTY, not inside a nested X11 session)
// --monitors-only  print the XRandR monitor layout and exit
//
// A model path may be passed like other examples; defaults to a built-in sphere.

#include "osgx/Core.hpp"
#include "osgx/Library.hpp"
#include "osgx/Linux.hpp"

#ifdef OSGX_EGL
#include "osgx/GraphicsWindowEGL.hpp"
#endif

#ifdef OSGX_GBM
#include "osgx/GraphicsWindowGBM.hpp"
#endif

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Shape>
#include <osg/ShapeDrawable>

#include <osgDB/ReadFile>

#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

osg::ref_ptr<osg::Node> createDefaultScene() {
	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(osgx::make_ref<osg::ShapeDrawable>(new osg::Sphere(osg::Vec3(), 5.0f)));

	return geode;
}

}

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	bool monitorsOnly = false;
	bool useEGL = false;
	bool useGBM = false;
	std::string modelPath;

	for(int i = 1; i < argc; i++) {
		std::string arg = argv[i];

		if(arg == "--monitors-only") monitorsOnly = true;
		else if(arg == "--egl") useEGL = true;
		else if(arg == "--gbm") useGBM = true;
		else modelPath = arg;
	}

	auto monitors = osgx::platform::listMonitors();

	std::cout << "XRandR monitors:" << std::endl;

	if(monitors.empty()) std::cout << "  (none found - no X display?)" << std::endl;

	for(const auto& mon : monitors) {
		std::cout
			<< "  " << (mon.primary ? "* " : "  ") << mon.name
			<< " " << mon.width << "x" << mon.height
			<< " @ (" << mon.x << ", " << mon.y << ")"
			<< std::endl
		;
	}

	if(monitorsOnly) return 0;

	if(useEGL && useGBM) {
		std::cerr << "--egl and --gbm are mutually exclusive" << std::endl;

		return 1;
	}

#ifndef OSGX_EGL
	if(useEGL) {
		std::cerr << "--egl requires osgx built with OSGX_WITH_EGL" << std::endl;

		return 1;
	}
#endif

#ifndef OSGX_GBM
	if(useGBM) {
		std::cerr << "--gbm requires osgx built with OSGX_WITH_GBM" << std::endl;

		return 1;
	}
#endif

	osg::ref_ptr<osg::Node> scene = modelPath.empty() ?
		createDefaultScene() :
		osgDB::readRefNodeFile(modelPath)
	;

	if(!scene) {
		std::cerr << "Failed to load: " << modelPath << std::endl;

		return 1;
	}

	osgViewer::Viewer viewer;

	viewer.setSceneData(scene);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());

#ifdef OSGX_EGL
	if(useEGL) {
		auto traits = osgx::make_ref<osg::GraphicsContext::Traits>();

		traits->width = 800;
		traits->height = 600;

		auto gc = osgx::platform::createEGLWindow(traits);

		if(!gc || !gc->valid()) {
			std::cerr << "Failed to create EGL window" << std::endl;

			return 1;
		}

		viewer.getCamera()->setGraphicsContext(gc);
		viewer.getCamera()->setViewport(0, 0, traits->width, traits->height);
		viewer.getCamera()->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER);

		std::cout << "Driving the window with EGL (X11 window, no GLX)." << std::endl;

		while(!viewer.done()) {
			viewer.frame();

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		return 0;
	}
#endif

#ifdef OSGX_GBM
	if(useGBM) {
		auto traits = osgx::make_ref<osg::GraphicsContext::Traits>();

		traits->width = 800;
		traits->height = 600;

		auto gc = osgx::platform::createGBMWindow(traits);

		if(!gc || !gc->valid()) {
			std::cerr
				<< "Failed to create GBM window (needs exclusive DRM access - try a bare TTY)"
				<< std::endl;

			return 1;
		}

		viewer.getCamera()->setGraphicsContext(gc);
		viewer.getCamera()->setViewport(0, 0, traits->width, traits->height);
		viewer.getCamera()->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER);

		std::cout << "Driving the window with GBM/DRM (direct scanout, no X11)." << std::endl;

		while(!viewer.done()) {
			viewer.frame();

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		return 0;
	}
#endif

	viewer.realize();

	if(!monitors.empty()) {
		const auto* primary = &monitors.front();

		for(const auto& mon : monitors) if(mon.primary) primary = &mon;

		osgx::platform::moveWindow(viewer, primary->x + 40, primary->y + 40);
	}

	osgx::platform::alwaysOnTop(viewer);

	return viewer.run();
}
