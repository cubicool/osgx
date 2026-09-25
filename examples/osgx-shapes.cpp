// vimrun! ./examples/osgx-shapes
//
// A deliberately plain gallery for osgx's flat-shaded polyhedra.  Keep new entries here as concrete shapes land
// in the library: it is both a visual smoke test for their topology/attribute layout and a small,
// readable starting point for applications that want the generated Geometry directly.

#include "osgx/Library.hpp"
#include "osgx/Shapes.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/GL>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Uniform>

#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <array>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view VERTEX_SHADER = R"GLSL(
#version 430 core

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;

uniform mat4 osg_ModelViewProjectionMatrix;
uniform mat3 osg_NormalMatrix;

out vec3 vNormal;

void main() {
	vNormal = normalize(osg_NormalMatrix * normal);
	gl_Position = osg_ModelViewProjectionMatrix * vec4(position, 1.0);
}
)GLSL";

constexpr std::string_view FRAGMENT_SHADER = R"GLSL(
#version 430 core

in vec3 vNormal;

uniform vec3 bodyColor;

out vec4 fragColor;

void main() {
	const vec3 lightDirection = vec3(0.4, 0.6, 0.7);
	float diffuse = max(dot(normalize(vNormal), normalize(lightDirection)), 0.0);
	float light = 0.35 + 0.65 * diffuse;

	fragColor = vec4(bodyColor * light, 1.0);
}
)GLSL";

struct Shape {
	std::string_view name;
	osg::Vec3 position;
	osg::Vec3 color;
	osg::ref_ptr<osg::Geometry> geometry;
};

osg::ref_ptr<osg::Program> makeProgram() {
	auto program = new osg::Program();

	program->addShader(new osg::Shader(osg::Shader::VERTEX, VERTEX_SHADER.data()));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, FRAGMENT_SHADER.data()));

	return program;
}

osg::ref_ptr<osg::Geode> makeShape(const Shape& entry, osg::Program* program) {
	auto geode = new osg::Geode();
	auto* stateSet = geode->getOrCreateStateSet();

	geode->setName(std::string(entry.name));
	geode->addDrawable(entry.geometry);
	stateSet->setAttributeAndModes(program, osg::StateAttribute::ON);
	stateSet->addUniform(new osg::Uniform("bodyColor", entry.color));

	return geode;
}

}

int main() {
	auto lib = osgx::initialize();

	auto root = new osg::Group();
	auto program = makeProgram();

	// Add one entry for every concrete osgx type.  This array grows with the library.
	const std::array shapes = {
		Shape{
			"Tetrahedron",
			osg::Vec3(-2.5f, 0.0f, 1.7f),
			osg::Vec3(0.12f, 0.78f, 0.34f),
			new osgx::Tetrahedron(osg::Vec3(), 1.2f)
		},
		Shape{
			"Cube",
			osg::Vec3(0.0f, 0.0f, 1.7f),
			osg::Vec3(0.52f, 0.55f, 0.61f),
			new osgx::Cube(osg::Vec3(), osg::Vec3(1.3856406f, 1.3856406f, 1.3856406f))
		},
		Shape{
			"Octahedron",
			osg::Vec3(2.5f, 0.0f, 1.7f),
			osg::Vec3(0.04f, 0.72f, 0.90f),
			new osgx::Octahedron(osg::Vec3(), 1.2f)
		},
		Shape{
			"Icosahedron",
			osg::Vec3(-1.6f, 0.0f, -1.7f),
			osg::Vec3(0.90f, 0.10f, 0.10f),
			new osgx::Icosahedron(osg::Vec3(), 1.2f)
		},
		Shape{
			"Dodecahedron",
			osg::Vec3(1.6f, 0.0f, -1.7f),
			osg::Vec3(0.96f, 0.78f, 0.08f),
			new osgx::Dodecahedron(osg::Vec3(), 1.2f)
		},
		Shape{
			"Pentagonal trapezohedron",
			osg::Vec3(4.4f, 0.0f, -1.7f),
			osg::Vec3(0.88f, 0.16f, 0.72f),
			new osgx::PentagonalTrapezohedron(osg::Vec3(), 1.2f)
		}
	};

	for(const auto& shape: shapes) {
		auto geode = makeShape(shape, program);
		auto transform = new osg::MatrixTransform(osg::Matrix::translate(shape.position));

		geode->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
		transform->addChild(geode);
		root->addChild(transform);
	}

	auto viewer = osgViewer::Viewer();

	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.getCamera()->setClearColor(osg::Vec4(0.04f, 0.05f, 0.10f, 1.0f));
	viewer.addEventHandler(new osgViewer::StatsHandler());

	return viewer.run();
}
