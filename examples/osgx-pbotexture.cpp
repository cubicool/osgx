// TEMPORARY: drives osgx::PBOTexture with a real WebM decode via mbew (~/dev/mbew, not vendored)
// instead of the procedural RGBA noise source this example started with, to see whether mbew is
// worth vendoring as an ext/ submodule. Revert to the noise FrameSource (see git history) if not.

#include "osgx/ImGui.hpp"
#include "osgx/PBOTexture.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Shape>
#include <osg/StateSet>
#include <osg/Timer>

#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

// TEMPORARY: not installed anywhere osgx searches by default - see the matching TEMPORARY block
// in examples/CMakeLists.txt that adds ~/dev/mbew/src to this target's include path.
#include <mbew.hpp>

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
	const std::string path = argc > 1 ? argv[1] : std::string(getenv("HOME")) + "/dev/mbew/data/small.webm";
	mbew::Context video = mbew::create(path);

	if(!video->valid()) {
		std::cerr << "Failed to open '" << path << "': " << mbew::string(video->status()) << std::endl;

		return 1;
	}

	if(!video->property(mbew::Property::VIDEO).b) {
		std::cerr << path << " contains no video track." << std::endl;

		return 1;
	}

	const int width = static_cast<int>(video->property(mbew::Property::VIDEO_WIDTH).num);
	const int height = static_cast<int>(video->property(mbew::Property::VIDEO_HEIGHT).num);

	auto texture = osgx::make_ref<osgx::PBOTexture>(
		width,
		height,
		// mbew_format_rgb() writes B,G,R,A per pixel - see ~/dev/mbew/src/mbew-format.c.
		static_cast<GLenum>(GL_BGRA),
		[video, time = osg::ElapsedTime()]() mutable -> const void* {
			if(!video->iterate(mbew::Iterate::VIDEO | mbew::Iterate::RGB | mbew::Iterate::SYNC)) {
				video->reset();
				time.reset();

				return nullptr;
			}

			if(!video->iter.sync(static_cast<mbew::ns_t>(time.elapsedTime_n()))) return nullptr;

			return video->iter.rgb();
		}
	);

	// Sized to the video's own aspect ratio rather than a fixed 1x1 quad, so it isn't stretched.
	const float aspect = static_cast<float>(width) / static_cast<float>(height);
	// b/t swapped (matching mbew-example-video-osg.cpp) - mbew's RGB buffer is row-major top-down,
	// but createTexturedQuadGeometry's default UVs put (0,0) at the quad's bottom-left corner, so
	// without this the video renders upside-down.
	auto quad = osg::createTexturedQuadGeometry(
		osg::Vec3(-0.5f * aspect, 0.0f, -0.5f),
		osg::Vec3(aspect, 0.0f, 0.0f),
		osg::Vec3(0.0f, 0.0f, 1.0f),
		0.0f, 1.0f, 1.0f, 0.0f
	);
	auto geode = osgx::make_ref<osg::Geode>();
	auto stateSet = geode->getOrCreateStateSet();

	geode->addDrawable(quad);
	stateSet->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
	stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF);

	osgViewer::Viewer viewer;

#ifdef OSGX_IMGUI
	// osgx::imgui::Widget requires this - see its class comment (ImGui's single global context
	// isn't safe to touch from more than one OSG draw thread).
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
#endif

	viewer.setSceneData(geode);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator);
	viewer.addEventHandler(new osgViewer::StatsHandler);
	viewer.setUpViewInWindow(80, 80, 800, 800);

#ifdef OSGX_IMGUI
	auto* gui = new osgx::imgui::Widget(viewer);

	gui->addSection("PBOTexture", [texture](osg::RenderInfo& ri) {
		const unsigned int contextID = ri.getState()->getContextID();

		ImGui::Text(
			"Upload (wait + memcpy + submit): %.3f ms",
			texture->getAverageUploadMilliseconds(contextID)
		);
		ImGui::Text(
			"  of which glClientWaitSync stall: %.3f ms",
			texture->getAverageWaitMilliseconds(contextID)
		);
	}, {.defaultOpen = true});
#else
	std::cerr << "OSGX_IMGUI not compiled in - skipping the profiler section." << std::endl;
#endif

	return viewer.run();
}
