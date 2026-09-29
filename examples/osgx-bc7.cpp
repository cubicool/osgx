// vimrun! ./osglibdir.sh ./examples/osgx-bc7 /tmp/boltgun.dds
//
// Displays a DDS DX10 BC7 texture through osgx::makeCompressedTexture2D(). Unlike osgviewerd's
// ordinary image path, this preserves the compressed blocks and authored mip chain on the GPU.
// Zoom with the trackball to inspect both the base level and mip filtering.

#include "osgx/CompressedTexture.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Shape>
#include <osg/StateSet>
#include <osg/Texture2D>

#include <osgDB/ReadFile>

#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
	if(argc != 2) {
		std::cerr << "Usage: " << argv[0] << " file.dds" << std::endl;

		return EXIT_FAILURE;
	}

	auto image = osgDB::readRefImageFile(argv[1]);

	if(!image) {
		std::cerr << "Failed to load '" << argv[1] << "'" << std::endl;

		return EXIT_FAILURE;
	}

	if(!osgx::isBC7(static_cast<GLenum>(image->getInternalTextureFormat()))) {
		std::cerr << "Expected a BC7 DDS image" << std::endl;

		return EXIT_FAILURE;
	}

	auto texture = osgx::makeCompressedTexture2D(image);

	texture->setDataVariance(osg::Object::STATIC);
	texture->setResizeNonPowerOfTwoHint(false);
	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

	const float width = static_cast<float>(image->s()) / static_cast<float>(image->t());
	auto quad = osg::createTexturedQuadGeometry(
		osg::Vec3(-width * 0.5f, 0.0f, -0.5f),
		osg::Vec3(width, 0.0f, 0.0f),
		osg::Vec3(0.0f, 0.0f, 1.0f)
	);
	auto geode = osgx::make_ref<osg::Geode>();
	auto stateSet = geode->getOrCreateStateSet();

	geode->addDrawable(quad);
	stateSet->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
	stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF);

	osgViewer::Viewer viewer;

	viewer.setSceneData(geode);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator);
	viewer.addEventHandler(new osgViewer::StatsHandler);
	viewer.setUpViewInWindow(80, 80, 1280, 720);

	return viewer.run();
}
