#pragma once

// Options every osgx example accepts, and the run() that applies them:
//
//   --headless [egl|native], --headless-frames N, --headless-out FILE
//                     Render offscreen (osgx::headless::readArguments()). Any osgx::imgui::Widget
//                     the example installed is removed first, so the image shows only the scene;
//                     --headless-imgui keeps it.
//   --camera-eye X,Y,Z, --camera-center X,Y,Z, --camera-up X,Y,Z
//                     The initial camera, e.g. to frame a headless capture. Each one left out keeps
//                     the camera manipulator's own home value (its computed home, framing the
//                     scene), or the camera's view matrix without a manipulator.
//   --profile         Attach the osgx::debug profiler. In a window (with ImGui): its waterfall as a
//                     section of the example's osgx::imgui::Widget, or of a new one. Headless or
//                     without ImGui: Profiler::reportWaterfall() on stdout once the run ends;
//                     headless renders PROFILE_FRAMES frames unless --headless-frames is given
//                     (the profile averages over 60 frames, and the first ones are warm-up).
//
// Usage:
//
//   int main(int argc, char** argv) {
//       osg::ArgumentParser args(&argc, argv);
//
//       const auto options = example::readArguments(args);
//
//       ... build the scene and the viewer, set its scene data ...
//
//       return example::run(viewer, options);
//   }

#include "osgx/Debug.hpp"
#include "osgx/Headless.hpp"

#ifdef OSGX_IMGUI
#include "osgx/ImGui.hpp"
#endif

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osgGA/CameraManipulator>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace example {

constexpr std::size_t PROFILE_FRAMES = 120;

struct Options {
	osgx::headless::Options headless;
	bool headlessImGui = false;
	bool profile = false;

	std::optional<osg::Vec3d> cameraEye;
	std::optional<osg::Vec3d> cameraCenter;
	std::optional<osg::Vec3d> cameraUp;
};

// Reads `name X,Y,Z` from `args`; throws std::invalid_argument for a malformed value.
inline std::optional<osg::Vec3d> readVec3(osg::ArgumentParser& args, const std::string& name) {
	std::string value;

	if(!args.read(name, value)) return std::nullopt;

	std::istringstream stream(value);
	osg::Vec3d result;
	char comma0 = 0;
	char comma1 = 0;

	stream >> result.x() >> comma0 >> result.y() >> comma1 >> result.z();

	if(!stream || comma0 != ',' || comma1 != ',' || !(stream >> std::ws).eof()) {
		throw std::invalid_argument(name + " expects X,Y,Z, got \"" + value + "\"");
	}

	return result;
}

// Removes the shared options from `args` (call first, before the example reads its own) and adds
// them to its usage text.
inline Options readArguments(osg::ArgumentParser& args) {
	auto* usage = args.getApplicationUsage();

	usage->addCommandLineOption("--headless [egl|native]", "Render offscreen (no window)");
	usage->addCommandLineOption("--headless-frames <N>", "Frames to render headless (default 10)");
	usage->addCommandLineOption("--headless-out <file>", "Write the last headless frame to <file>");
	usage->addCommandLineOption("--headless-imgui", "Keep the example's ImGui panel when headless");
	usage->addCommandLineOption(
		"--profile",
		"Profile with osgx::debug (ImGui waterfall in a window; text waterfall when headless)"
	);
	usage->addCommandLineOption("--camera-eye <X,Y,Z>", "Initial camera eye");
	usage->addCommandLineOption("--camera-center <X,Y,Z>", "Initial camera look-at center");
	usage->addCommandLineOption("--camera-up <X,Y,Z>", "Initial camera up vector");

	Options options;

	const bool framesGiven = args.find("--headless-frames") >= 0;

	options.headless = osgx::headless::readArguments(args);
	options.headlessImGui = args.read("--headless-imgui");
	options.profile = args.read("--profile");
	options.cameraEye = readVec3(args, "--camera-eye");
	options.cameraCenter = readVec3(args, "--camera-center");
	options.cameraUp = readVec3(args, "--camera-up");

	if(options.profile && !framesGiven) options.headless.frames = PROFILE_FRAMES;

	return options;
}

#ifdef OSGX_IMGUI
// The osgx::imgui::Widget among `view`'s event handlers (a Widget adds itself there), if any.
inline osgx::imgui::Widget* findWidget(osgViewer::View& view) {
	for(auto& handler : view.getEventHandlers()) {
		if(auto* widget = dynamic_cast<osgx::imgui::Widget*>(handler.get())) return widget;
	}

	return nullptr;
}
#endif

// Applies --camera-eye/--camera-center/--camera-up: as the manipulator's (now fixed) home position,
// or directly as the camera's view matrix without a manipulator.
inline void applyCamera(osgViewer::Viewer& viewer, const Options& options) {
	if(!options.cameraEye && !options.cameraCenter && !options.cameraUp) return;

	osg::Vec3d eye;
	osg::Vec3d center;
	osg::Vec3d up;

	auto* manipulator = viewer.getCameraManipulator();

	if(manipulator) {
		if(manipulator->getAutoComputeHomePosition() && manipulator->getNode()) {
			manipulator->computeHomePosition(viewer.getCamera());
		}

		manipulator->getHomePosition(eye, center, up);
	}

	else viewer.getCamera()->getViewMatrixAsLookAt(eye, center, up);

	if(options.cameraEye) eye = *options.cameraEye;
	if(options.cameraCenter) center = *options.cameraCenter;
	if(options.cameraUp) up = *options.cameraUp;

	if(manipulator) {
		manipulator->setAutoComputeHomePosition(false);
		manipulator->setHomePosition(eye, center, up);
		manipulator->home(0.0);
	}

	else viewer.getCamera()->setViewMatrixAsLookAt(eye, center, up);
}

// Runs `viewer` (its scene data set, not yet realized) with the shared options; returns its exit
// code. Replaces viewer.run() / osgx::headless::run().
inline int run(osgViewer::Viewer& viewer, const Options& options) {
	osg::ref_ptr<osgx::debug::Profiler> profiler;

#ifdef OSGX_IMGUI
	// The Widget only installs its draw callbacks from its first event, so removing it before the
	// run leaves no trace of it in the frame.
	if(options.headless.enabled && !options.headlessImGui) {
		if(auto* widget = findWidget(viewer)) viewer.removeEventHandler(widget);
	}
#endif

	applyCamera(viewer, options);

	if(options.profile) {
#ifdef OSGX_IMGUI
		if(!options.headless.enabled) {
			auto* widget = findWidget(viewer);

			// Dear ImGui's one global context must only be touched from one thread; an example
			// with its own Widget has already chosen this.
			if(!widget) {
				viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);

				widget = new osgx::imgui::Widget(viewer);
			}

			widget->addProfilerSection(viewer, true);
		}

		else
#endif
		{
			profiler = new osgx::debug::Profiler();

			profiler->attach(viewer);
		}
	}

	const int result = osgx::headless::run(viewer, options.headless);

	if(profiler) profiler->reportWaterfall(std::cout);

	return result;
}

}
