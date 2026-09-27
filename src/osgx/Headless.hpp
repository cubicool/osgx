#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/GraphicsContext>
#include <osg/ref_ptr>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <filesystem>
#include <optional>

namespace osgx::headless {

enum class Backend {
	AUTO,
	EGL,
	NATIVE
};

struct Context {
	osg::ref_ptr<osg::GraphicsContext> graphicsContext;
	Backend backend = Backend::AUTO;
};

// The command-line part of headless rendering. readArguments() removes its options from arguments
// so an application's own parser and osgViewer::Viewer never see them.
struct Options {
	bool enabled = false;
	Backend backend = Backend::AUTO;
	std::size_t frames = 10;
	std::filesystem::path output;
	int width = 800;
	int height = 600;
};

// Consume --headless [egl|native], --headless-frames N, and --headless-out FILE. Backend aliases
// are auto, glx, wgl, and cocoa. Throws std::invalid_argument for malformed headless options.
Options readArguments(osg::ArgumentParser& arguments);

// Create an offscreen pbuffer. AUTO selects EGL when osgx was built with OSGX_EGL and otherwise
// uses OSG's native pbuffer implementation. samples overrides DisplaySettings when supplied.
Context createContext(
	int width,
	int height,
	std::optional<unsigned int> samples=std::nullopt,
	Backend backend=Backend::AUTO
);

// Run viewer normally unless options.enabled. Call before viewer.realize(). Headless execution
// replaces the master camera's context, renders options.frames frames single-threaded, writes
// options.output, and returns 1 if context creation or final-frame capture fails.
int run(osgViewer::Viewer& viewer, const Options& options);

}
