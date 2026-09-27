#include "osgx/Headless.hpp"

#ifdef OSGX_EGL
#include "osgx/GraphicsWindowEGL.hpp"
#endif

OSGX_DISABLE_WARNINGS

#include <osg/DisplaySettings>
#include <osg/GL>
#include <osg/Image>
#include <osg/Viewport>
#include <osgDB/WriteFile>
#include <osgViewer/ViewerBase>

OSGX_ENABLE_WARNINGS

#include <charconv>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace osgx::headless {

namespace {

Backend parseBackend(std::string_view value) {
	if(value == "auto") return Backend::AUTO;
	if(value == "egl") return Backend::EGL;
	if(value == "native" || value == "glx" || value == "wgl" || value == "cocoa") return Backend::NATIVE;

	throw std::invalid_argument(
		"unknown headless backend '" + std::string(value)
		+ "' (expected egl, native, auto, glx, wgl, or cocoa)"
	);
}

std::size_t parseFrames(std::string_view value) {
	std::size_t frames = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frames);
	if(error != std::errc() || end != value.data() + value.size() || frames == 0) {
		throw std::invalid_argument("--headless-frames must be at least 1");
	}

	return frames;
}

const char* backendName(Backend backend) {
	switch(backend) {
		case Backend::EGL: return "egl";
		case Backend::NATIVE: return "native";
		case Backend::AUTO: return "auto";
	}

	return "unknown";
}

std::string glString(GLenum name) {
	const auto* value = glGetString(name);

	return value ? reinterpret_cast<const char*>(value) : "";
}

class CaptureCallback: public osg::Camera::DrawCallback {
public:
	CaptureCallback(
		osg::Camera::DrawCallback* previous,
		std::filesystem::path output,
		int width,
		int height
	):
	_previous(previous),
	_output(std::move(output)),
	_width(width),
	_height(height) {}

	void operator()(osg::RenderInfo& renderInfo) const override {
		if(_previous) (*_previous)(renderInfo);

		auto image = osgx::make_ref<osg::Image>();

		image->readPixels(0, 0, _width, _height, GL_RGB, GL_UNSIGNED_BYTE);

		_written = osgDB::writeImageFile(*image, _output.string());
		_captured = true;
		_blank = isBlank(*image);
		_vendor = glString(GL_VENDOR);
		_renderer = glString(GL_RENDERER);
		_version = glString(GL_VERSION);
	}

	bool captured() const { return _captured; }
	bool written() const { return _written; }
	bool blank() const { return _blank; }
	osg::Camera::DrawCallback* previous() const { return _previous.get(); }
	const std::string& vendor() const { return _vendor; }
	const std::string& renderer() const { return _renderer; }
	const std::string& version() const { return _version; }

private:
	static bool isBlank(const osg::Image& image) {
		const auto size = image.getTotalSizeInBytes();
		const auto* pixels = image.data();

		if(size < 3 || !pixels) return true;

		for(std::size_t index = 3; index < size; index += 3) {
			if(std::memcmp(pixels, pixels + index, 3) != 0) return false;
		}

		return true;
	}

	osg::ref_ptr<osg::Camera::DrawCallback> _previous;
	std::filesystem::path _output;

	int _width;
	int _height;
	mutable bool _captured = false;
	mutable bool _written = false;
	mutable bool _blank = false;
	mutable std::string _vendor;
	mutable std::string _renderer;
	mutable std::string _version;
};

}

Options readArguments(osg::ArgumentParser& arguments) {
	Options options;
	const auto applicationName = std::filesystem::path(arguments.getApplicationName());
	options.output = applicationName.stem().string() + "-headless.png";

	for(int index = 1; index < arguments.argc();) {
		const std::string_view argument(arguments[index]);
		const auto equal = argument.find('=');
		const std::string name(argument.substr(0, equal));
		const std::string value = equal == std::string_view::npos ? "" : std::string(argument.substr(equal + 1));

		if(name == "--headless") {
			options.enabled = true;

			arguments.remove(index);

			if(equal != std::string_view::npos) options.backend = parseBackend(value);

			else if(index < arguments.argc()) {
				const std::string_view next(arguments[index]);

				if(next == "egl" || next == "native" || next == "auto" || next == "glx" || next == "wgl" || next == "cocoa") {
					options.backend = parseBackend(next);

					arguments.remove(index);
				}
			}

			continue;
		}

		if(name == "--headless-frames" || name == "--headless-out") {
			std::string optionValue(value);

			arguments.remove(index);

			if(equal == std::string_view::npos) {
				if(index >= arguments.argc()) throw std::invalid_argument(std::string(name) + " needs a value");

				optionValue = arguments[index];

				arguments.remove(index);
			}

			if(name == "--headless-frames") options.frames = parseFrames(optionValue);

			else options.output = optionValue;

			continue;
		}

		index++;
	}

	return options;
}

Context createContext(int width, int height, std::optional<unsigned int> samples, Backend backend) {
	auto traits = osgx::make_ref<osg::GraphicsContext::Traits>(osg::DisplaySettings::instance());

	traits->width = width;
	traits->height = height;
	traits->pbuffer = true;
	traits->doubleBuffer = false;

	if(samples) {
		traits->sampleBuffers = *samples ? 1u : 0u;
		traits->samples = *samples;
	}

	Context context;
	if(backend != Backend::NATIVE) {
#ifdef OSGX_EGL
		context.graphicsContext = osgx::platform::createEGLWindow(traits);

		if(context.graphicsContext.valid() && context.graphicsContext->valid()) {
			context.backend = Backend::EGL;

			return context;
		}

		if(backend == Backend::EGL) throw std::runtime_error("EGL pbuffer context could not be created");
#else
		if(backend == Backend::EGL) throw std::runtime_error("the egl backend requires osgx built with OSGX_EGL");
#endif
	}

	traits->readDISPLAY();

	context.graphicsContext = osg::GraphicsContext::createGraphicsContext(traits);
	context.backend = Backend::NATIVE;

	if(!context.graphicsContext.valid() || !context.graphicsContext->valid()) {
		throw std::runtime_error("native pbuffer context could not be created");
	}

	return context;
}

int run(osgViewer::Viewer& viewer, const Options& options) {
	if(!options.enabled) return viewer.run();

	Context context;

	try {
		context = createContext(options.width, options.height, std::nullopt, options.backend);
	}

	catch(const std::exception& exception) {
		OSG_WARN << "headless: FAIL - " << exception.what() << std::endl;

		return 1;
	}

	auto* camera = viewer.getCamera();

	viewer.setThreadingModel(osgViewer::ViewerBase::SingleThreaded);
	camera->setGraphicsContext(context.graphicsContext);
	camera->setViewport(new osg::Viewport(0, 0, options.width, options.height));
	camera->setProjectionMatrixAsPerspective(
		30.0,
		static_cast<double>(options.width) / static_cast<double>(options.height),
		1.0,
		10000.0
	);

	OSG_NOTICE
		<< "headless: " << backendName(context.backend) << " pbuffer "
		<< options.width << 'x' << options.height << ", " << options.frames << " frames -> "
		<< std::filesystem::absolute(options.output).string() << std::endl;

	for(std::size_t frame = 0; frame < options.frames; frame++) {
		if(frame + 1 != options.frames) {
			viewer.frame();

			continue;
		}

		auto capture = osgx::make_ref<CaptureCallback>(
			camera->getFinalDrawCallback(), options.output, options.width, options.height
		);
		camera->setFinalDrawCallback(capture);
		viewer.frame();
		camera->setFinalDrawCallback(capture->previous());

		if(!capture->captured()) {
			OSG_WARN << "headless: FAIL - the last frame never reached the final draw callback" << std::endl;

			return 1;
		}

		OSG_NOTICE << "headless: GL vendor:   " << capture->vendor() << std::endl;
		OSG_NOTICE << "headless: GL renderer: " << capture->renderer() << std::endl;
		OSG_NOTICE << "headless: GL version:  " << capture->version() << std::endl;

		if(!capture->written()) {
			OSG_WARN << "headless: FAIL - could not write " << options.output.string() << std::endl;

			return 1;
		}

		if(capture->blank()) {
			OSG_WARN << "headless: every pixel is identical in " << options.output.string() << std::endl;
		}

		else {
			OSG_NOTICE << "headless: wrote " << std::filesystem::absolute(options.output).string() << std::endl;
		}
	}

	viewer.setDone(true);

	return 0;
}

}
