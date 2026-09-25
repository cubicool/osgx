// vimrun! ./examples/osgx-imgui

#include "osgx/Core.hpp"
#include "osgx/ImGui.hpp"
#include "osgx/Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Group>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osg/Texture2D>

#include <osgGA/StateSetManipulator>
#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

namespace {
	osg::Image* makeProceduralImage(int index) {
		constexpr std::size_t width = 64;
		constexpr std::size_t height = 64;

		auto image = osgx::make_nref<osg::Image>("ProceduralImage_" + std::to_string(index));

		image->allocateImage(
			static_cast<int>(width),
			static_cast<int>(height),
			1,
			GL_RGBA,
			GL_UNSIGNED_BYTE
		);

		for(std::size_t y = 0; y < height; ++y) {
			for(std::size_t x = 0; x < width; ++x) {
				auto* px = image->data(
					static_cast<unsigned int>(x),
					static_cast<unsigned int>(y)
				);
				const bool checker = ((x / 8) + (y / 8)) % 2 == 0;

				px[0] = static_cast<unsigned char>((x * 255) / (width - 1));
				px[1] = static_cast<unsigned char>((y * 255) / (height - 1));
				px[2] = static_cast<unsigned char>(checker ? 255 - index * 60 : 40 + index * 70);
				px[3] = 255;
			}
		}

		return image.release();
	}

	osg::Texture2D* makeProceduralTexture(int index) {
		auto texture = osgx::make_nref<osg::Texture2D>("ProceduralTexture_" + std::to_string(index));

		texture->setImage(makeProceduralImage(index));
		texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
		texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
		texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
		texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

		return texture.release();
	}
}

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	osgViewer::Viewer viewer;

	// osgx::imgui::Widget no longer checks/forces this itself (see its class
	// comment) - Dear ImGui's single global context isn't safe to touch from more
	// than one OSG draw thread, so this is the caller's responsibility now, same
	// as osgEarth's own ImGuiEventHandler.
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);

	// Build a small scene: three named spheres at different X positions.
	auto root = osgx::make_nref<osg::Group>("Root");

	// Each sphere gets a progressively higher tessellation detail ratio so they
	// have genuinely different GPU costs - a ground-truth check for the profiler.
	static constexpr float detailRatios[] = { 0.5f, 8.0f, 16.0f };

	for(int i = 0; i < 3; i++) {
		auto xform = osgx::make_nref<osg::MatrixTransform>(
			"Transform_" + std::to_string(i),
			osg::Matrix::translate(osg::Vec3(static_cast<float>(i) * 12.0f, 0.0f, 0.0f))
		);

		auto geode = osgx::make_nref<osg::Geode>("Geode_" + std::to_string(i));
		auto hints = osgx::make_ref<osg::TessellationHints>();

		hints->setDetailRatio(detailRatios[i]);

		auto sphere = osgx::make_ref<osg::ShapeDrawable>(
			new osg::Sphere(osg::Vec3(), 5.0f),
			hints
		);

		sphere->setName("Sphere_x" + std::to_string(static_cast<int>(detailRatios[i])));
		sphere->getOrCreateStateSet()->setTextureAttributeAndModes(
			0,
			makeProceduralTexture(i),
			osg::StateAttribute::ON
		);

		geode->addDrawable(sphere);
		xform->addChild(geode);
		root->addChild(xform);
	}

	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));

	// Widget pushes itself to the front of the viewer's event handler list
	// automatically (SingleThreaded is our responsibility now, set above).
	auto* gui = new osgx::imgui::Widget(viewer);

	gui->addStatsSection(viewer);
	gui->addProfilerSection(viewer, root);
	gui->addTextureSection(viewer, root);

	// Optional: append app-specific sections below the built-in ones.
	gui->addSection("My App", [](osg::RenderInfo&) {
		ImGui::Text("hello from user app");
	});

	return viewer.run();
}
