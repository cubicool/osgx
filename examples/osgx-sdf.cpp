// vimrun! ./examples/osgx-sdf sdf/msdf-atlas.gltf
//
// Renders BAKED distance-field textures through osgx::SDF, the StateAttribute + `#pragma osgx::sdf
// SAMPLING,TEXTURE` shader library. osgx only CONSUMES distance fields; generate the inputs
// elsewhere. Two modes:
//
//   osgx-sdf <atlas.gltf>
//     Loads an `osgx_sdf` glTF manifest (osgx::gltf::sdf::TileSet) and draws EVERY tile in a row,
//     all sharing the one atlas texture. slughorn writes these: `bin/slughorn sdf x.slug -o a.png`
//     produces a.png + a.gltf.
//
//   osgx-sdf <sdf.png> <msdf.png> [pixelRange=8]
//     Two raw fields (e.g. straight from the msdfgen CLI), a single-channel SDF (left) and a
//     3-channel MSDF (right). The pixelRange MUST match the -pxrange the fields were generated with:
//     it is what lets osgx_SDF_Coverage() turn the sampled distance into a screen-space
//     antialiasing ramp.
//
// Zoom in and out with the trackball - edges should stay crisp and evenly antialiased at any scale.

#include "osgx/Core.hpp"
#include "osgx/SDF.hpp"
#include "osgx/Shader.hpp"
#include "osgx/gltf/SDF.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BlendFunc>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>

#include <osgDB/ReadFile>

#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Generic vertex attributes at explicit locations, same as osgx-shapes.cpp.
constexpr const char* VERTEX_SHADER = R"GLSL(
#version 430 core

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;

uniform mat4 osg_ModelViewProjectionMatrix;

out vec2 vUV;

void main() {
	vUV = uv;
	gl_Position = osg_ModelViewProjectionMatrix * vec4(position, 1.0);
}
)GLSL";

// SAMPLING must precede TEXTURE - see registerSDFShaderLibs() in SDF.hpp. `#version 430` is
// required by the layout(binding=...) qualifiers TEXTURE declares.
constexpr const char* FRAGMENT_SHADER = R"GLSL(
#version 430 core

#pragma osgx::sdf SAMPLING,TEXTURE

in vec2 vUV;

uniform vec4 fillColor;

out vec4 fragColor;

void main() {
	float coverage = osgx_SDF_Coverage(vUV);

	fragColor = vec4(fillColor.rgb, fillColor.a * coverage);
}
)GLSL";

osg::ref_ptr<osg::Texture2D> loadTexture(const char* path) {
	auto image = osgDB::readRefImageFile(path);

	if(!image) {
		std::cerr << "Could not load image: " << path << std::endl;

		std::exit(1);
	}

	// Bilinear, no mipmaps, clamped, and the single-channel-PNG (GL_LUMINANCE) relabel - all of it
	// lives in osgx::SDF::makeTexture() so no caller has to know.
	return osgx::SDF::makeTexture(image);
}

// A quad in the XZ plane (faces the trackball's default -Y eye), `width` wide and 1 tall, its left
// edge at x and vertically centered on z = 0. UV (0, 0) is the bottom-left corner.
osg::ref_ptr<osg::Geometry> makeQuad(float x, float width) {
	auto positions = new osg::Vec3Array();
	auto uvs = new osg::Vec2Array();

	positions->push_back(osg::Vec3(x, 0.0f, -0.5f));
	positions->push_back(osg::Vec3(x + width, 0.0f, -0.5f));
	positions->push_back(osg::Vec3(x + width, 0.0f, 0.5f));
	positions->push_back(osg::Vec3(x, 0.0f, 0.5f));

	uvs->push_back(osg::Vec2(0.0f, 0.0f));
	uvs->push_back(osg::Vec2(1.0f, 0.0f));
	uvs->push_back(osg::Vec2(1.0f, 1.0f));
	uvs->push_back(osg::Vec2(0.0f, 1.0f));

	positions->setBinding(osg::Array::BIND_PER_VERTEX);
	uvs->setBinding(osg::Array::BIND_PER_VERTEX);

	auto geometry = new osg::Geometry();

	geometry->setUseDisplayList(false);
	geometry->setUseVertexBufferObjects(true);
	geometry->setVertexArray(positions);
	geometry->setVertexAttribArray(0, positions);
	geometry->setVertexAttribArray(1, uvs);
	geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN, 0, 4));

	return geometry;
}

osg::ref_ptr<osg::Geode> makeField(
	osg::Program* program,
	osgx::SDF* sdf,
	float x,
	float width,
	const osg::Vec4& color
) {
	auto geode = new osg::Geode();
	auto stateSet = geode->getOrCreateStateSet();

	geode->addDrawable(makeQuad(x, width));

	stateSet->setAttributeAndModes(program, osg::StateAttribute::ON);
	stateSet->setAttributeAndModes(sdf, osg::StateAttribute::ON);
	stateSet->addUniform(new osg::Uniform("fillColor", color));

	return geode;
}

}

int main(int argc, char** argv) {
	if(argc < 2) {
		std::cerr
			<< "usage: osgx-sdf <atlas.gltf>\n"
			<< "       osgx-sdf <sdf.png> <msdf.png> [pixelRange=8]" << std::endl
		;

		return 1;
	}

	// osgx::SDF's constructor registers the `osgx::sdf` shader-library catalog, so once ANY osgx::SDF
	// exists the pragma in FRAGMENT_SHADER is resolvable; both modes below build their attributes
	// before the Program.
	struct Entry {
		osg::ref_ptr<osgx::SDF> sdf;
		float width = 1.0f; // quad width for a 1-tall quad, i.e. the tile's aspect ratio
	};

	std::vector<Entry> entries;

	const std::string first = argv[1];
	const bool manifestMode = first.size() > 5 && first.substr(first.size() - 5) == ".gltf";

	if(manifestMode) {
		const auto tiles = osgx::gltf::sdf::TileSet::load(first);

		if(!tiles.valid()) {
			std::cerr << "Could not load an osgx_sdf manifest from " << first << std::endl;

			return 1;
		}

		std::cout << tiles.names().size() << " tile(s), "
			<< (tiles.sdfType() == osgx::SDF::SDFType::MSDF ? "MSDF" : "SDF") << ":";

		for(const auto& name : tiles.names()) {
			const auto& tile = tiles.tile(name);

			std::cout << ' ' << name << " (" << tile.w << 'x' << tile.h << ", pixelRange " << tile.pixelRange << ')';

			entries.push_back({tiles.attribute(name), static_cast<float>(tile.w) / static_cast<float>(tile.h)});
		}

		std::cout << std::endl;
	}

	else {
		if(argc < 3) {
			std::cerr << "usage: osgx-sdf <sdf.png> <msdf.png> [pixelRange=8]" << std::endl;

			return 1;
		}

		const float pixelRange = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 8.0f;

		for(int i = 0; i < 2; i++) {
			auto sdf = new osgx::SDF();

			sdf->setTexture(loadTexture(argv[1 + i]));
			sdf->setSDFType(i == 0 ? osgx::SDF::SDFType::SDF : osgx::SDF::SDFType::MSDF);
			sdf->setPixelRange(pixelRange);

			entries.push_back({sdf, 1.0f});
		}
	}

	auto program = new osg::Program();

	program->addShader(new osg::Shader(osg::Shader::VERTEX, VERTEX_SHADER));
	program->addShader(new osg::Shader(
		osg::Shader::FRAGMENT,
		osgx::resolveShaderLibs(FRAGMENT_SHADER)
	));

	static const osg::Vec4 COLORS[] = {
		osg::Vec4(1.0f, 0.75f, 0.2f, 1.0f),
		osg::Vec4(0.3f, 0.8f, 1.0f, 1.0f),
		osg::Vec4(0.6f, 1.0f, 0.5f, 1.0f),
		osg::Vec4(1.0f, 0.5f, 0.7f, 1.0f)
	};

	// One row, centered on the origin, 0.2 apart.
	const float gap = 0.2f;
	float total = -gap;

	for(const auto& entry : entries) total += entry.width + gap;

	auto root = new osg::Group();
	float x = -total * 0.5f;

	for(std::size_t i = 0; i < entries.size(); i++) {
		root->addChild(makeField(program, entries[i].sdf, x, entries[i].width, COLORS[i % 4]));

		x += entries[i].width + gap;
	}

	auto stateSet = root->getOrCreateStateSet();

	stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
	stateSet->setAttributeAndModes(
		new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
		osg::StateAttribute::ON
	);

	auto viewer = osgViewer::Viewer();

	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.getCamera()->setClearColor(osg::Vec4(0.04f, 0.05f, 0.10f, 1.0f));
	viewer.addEventHandler(new osgViewer::StatsHandler());

	return viewer.run();
}
