#pragma once

// Options every osgx example accepts, and the run() that applies them:
//
//   --headless [egl|native], --headless-frames N, --headless-out FILE
//                     Render offscreen (osgx::headless::readArguments()).
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
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <iostream>

namespace example {

constexpr std::size_t PROFILE_FRAMES = 120;

struct Options {
	osgx::headless::Options headless;
	bool profile = false;
};

// Removes the shared options from `args` (call first, before the example reads its own) and adds
// them to its usage text.
inline Options readArguments(osg::ArgumentParser& args) {
	auto* usage = args.getApplicationUsage();

	usage->addCommandLineOption("--headless [egl|native]", "Render offscreen (no window)");
	usage->addCommandLineOption("--headless-frames <N>", "Frames to render headless (default 10)");
	usage->addCommandLineOption("--headless-out <file>", "Write the last headless frame to <file>");
	usage->addCommandLineOption(
		"--profile",
		"Profile with osgx::debug (ImGui waterfall in a window; text waterfall when headless)"
	);

	Options options;

	const bool framesGiven = args.find("--headless-frames") >= 0;

	options.headless = osgx::headless::readArguments(args);
	options.profile = args.read("--profile");

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

// Runs `viewer` (its scene data set, not yet realized) with the shared options; returns its exit
// code. Replaces viewer.run() / osgx::headless::run().
inline int run(osgViewer::Viewer& viewer, const Options& options) {
	osg::ref_ptr<osgx::debug::Profiler> profiler;

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
