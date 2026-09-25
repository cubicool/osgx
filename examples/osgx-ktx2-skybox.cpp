// ktx2-skybox - load a KTX2 cubemap and display it as a skybox.
//
// Usage: ktx2-skybox <file.ktx2>
// +/- or ,/. : step mip level up/down (verify mip chain)
// scroll wheel : also adjusts mip level
// c : toggle flat-cross display

#include "osgx/Library.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osg/StateSet>
#include <osg/Switch>
#include <osg/TextureCubeMap>
#include <osg/Uniform>
#include <osgDB/ReadFile>
#include <osgGA/GUIEventHandler>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <iomanip>
#include <iostream>

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static const char* SKYBOX_VERT = R"GLSL(
#version 460 core
in vec4 osg_Vertex;
uniform mat4 osg_ModelViewProjectionMatrix;
out vec3 vDir;
void main() {
	vDir = osg_Vertex.xyz;
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
)GLSL";

static const char* SKYBOX_FRAG = R"GLSL(
#version 460 core
uniform samplerCube envMap;
uniform float mipLevel;
in vec3 vDir;
out vec4 fragColor;
void main() {
	vec3 color = textureLod(envMap, normalize(vDir), mipLevel).rgb;
	// Reinhard tone map + gamma - cubemap is linear HDR
	color = color / (color + vec3(1.0));
	color = pow(color, vec3(1.0 / 2.2));
	fragColor = vec4(color, 1.0);
}
)GLSL";

// Vertex positions are already in NDC; faceDir carries the cubemap direction.
static const char* CROSS_VERT = R"GLSL(
#version 460 core
in vec4 osg_Vertex;
layout(location = 1) in vec3 faceDir;
out vec3 vDir;
void main() {
	vDir = faceDir;
	gl_Position = vec4(osg_Vertex.xy, 0.0, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Cross geometry builder
// ---------------------------------------------------------------------------

// 6 quads in NDC, arranged as a horizontal cross (4 cols x 3 rows).
// Fills a 4:3 window exactly; faces appear square.
//
// [+Y] row 2
// [-X] [+Z] [+X] [-Z] row 1
// [-Y] row 0
//
// Directions per vertex follow the OpenGL cubemap spec:
// dir(s,t) = MA + (2s-1)*SC + (2t-1)*TC
// with BL=(s=0,t=0), BR=(s=1,t=0), TR=(s=1,t=1), TL=(s=0,t=1).
static osg::Geometry* buildCrossGeometry() {
	auto* geom = new osg::Geometry();
	auto* verts = new osg::Vec3Array();
	auto* dirs = new osg::Vec3Array();
	auto* elems = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);

	// 4 columns -> half-cell-width 0.25; 3 rows -> half-cell-height 1/3
	const float cw = 0.25f;
	const float ch = 1.0f / 3.0f;

	struct FaceData {
		float cx, cy; // NDC center
		osg::Vec3 dir[4]; // BL, BR, TR, TL
	};

	// Column centers: -0.75, -0.25, 0.25, 0.75
	// Row centers: -2/3, 0, +2/3
	FaceData faces[] = {
		// +Y : SC=+X, TC=+Z, MA=+Y -> (2s-1, 1, 2t-1)
		{ -0.25f, 2.0f/3.0f, {
			{-1, 1,-1}, {1, 1,-1}, {1, 1, 1}, {-1, 1, 1}
		}},
		// -X : SC=+Z, TC=-Y, MA=-X -> (-1, 1-2t, 2s-1)
		{ -0.75f, 0.0f, {
			{-1, 1,-1}, {-1, 1, 1}, {-1,-1, 1}, {-1,-1,-1}
		}},
		// +Z : SC=+X, TC=-Y, MA=+Z -> (2s-1, 1-2t, 1)
		{ -0.25f, 0.0f, {
			{-1, 1, 1}, {1, 1, 1}, {1,-1, 1}, {-1,-1, 1}
		}},
		// +X : SC=-Z, TC=-Y, MA=+X -> (1, 1-2t, 1-2s)
		{ 0.25f, 0.0f, {
			{1, 1, 1}, {1, 1,-1}, {1,-1,-1}, {1,-1, 1}
		}},
		// -Z : SC=-X, TC=-Y, MA=-Z -> (1-2s, 1-2t, -1)
		{ 0.75f, 0.0f, {
			{1, 1,-1}, {-1, 1,-1}, {-1,-1,-1}, {1,-1,-1}
		}},
		// -Y : SC=+X, TC=-Z, MA=-Y -> (2s-1, -1, 1-2t)
		{ -0.25f, -2.0f/3.0f, {
			{-1,-1, 1}, {1,-1, 1}, {1,-1,-1}, {-1,-1,-1}
		}},
	};

	for(auto& f : faces) {
		auto base = static_cast<unsigned>(verts->size());

		verts->push_back({f.cx - cw, f.cy - ch, 0.0f}); // BL
		verts->push_back({f.cx + cw, f.cy - ch, 0.0f}); // BR
		verts->push_back({f.cx + cw, f.cy + ch, 0.0f}); // TR
		verts->push_back({f.cx - cw, f.cy + ch, 0.0f}); // TL

		for(int i = 0; i < 4; ++i) dirs->push_back(f.dir[i]);

		elems->push_back(base + 0); elems->push_back(base + 1); elems->push_back(base + 2);
		elems->push_back(base + 0); elems->push_back(base + 2); elems->push_back(base + 3);
	}

	geom->setVertexArray(verts);
	geom->setVertexAttribArray(1, dirs);
	geom->setVertexAttribBinding(1, osg::Geometry::BIND_PER_VERTEX);
	geom->addPrimitiveSet(elems);

	return geom;
}

// ---------------------------------------------------------------------------
// Key handlers
// ---------------------------------------------------------------------------

class MipHandler: public osgGA::GUIEventHandler {
	osg::ref_ptr<osg::Uniform> _mipUniform;
	float _mipLevel = 0.0f;
	int _maxMip = 0;

	void _clamp() { _mipLevel = std::max(0.0f, std::min(float(_maxMip), _mipLevel)); }

	void _apply() {
		_mipUniform->set(_mipLevel);

		std::cout
			<< " mip level: " << std::fixed << std::setprecision(1) << _mipLevel
			<< " / " << _maxMip << std::defaultfloat << std::endl;
	}

public:
	MipHandler(osg::Uniform* u, int maxMip): _mipUniform(u), _maxMip(maxMip) {}

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override {
		if(ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN) {
			if(ea.getKey() == '+' || ea.getKey() == '=') { ++_mipLevel; _clamp(); _apply(); return true; }
			if(ea.getKey() == '-' || ea.getKey() == '_') { --_mipLevel; _clamp(); _apply(); return true; }
			if(ea.getKey() == '.' || ea.getKey() == '>') { _mipLevel += 0.25f; _clamp(); _apply(); return true; }
			if(ea.getKey() == ',' || ea.getKey() == '<') { _mipLevel -= 0.25f; _clamp(); _apply(); return true; }
		}

		if(ea.getEventType() == osgGA::GUIEventAdapter::SCROLL) {
			if(ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_UP) { _mipLevel += 0.5f; _clamp(); _apply(); return true; }
			if(ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_DOWN) { _mipLevel -= 0.5f; _clamp(); _apply(); return true; }
		}

		return false;
	}
};

class ModeHandler: public osgGA::GUIEventHandler {
	osg::ref_ptr<osg::Switch> _switch;
	bool _cross = false;

public:
	ModeHandler(osg::Switch* sw): _switch(sw) {}

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override {
		if(ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN && ea.getKey() == 'c') {
			_cross = !_cross;
			_switch->setValue(0, !_cross);
			_switch->setValue(1, _cross);

			std::cout << " mode: " << (_cross ? "cross" : "skybox") << std::endl;

			return true;
		}

		return false;
	}
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
	auto lib = osgx::initialize();

	if(argc < 2) {
		std::cerr
			<< "Usage: ktx2-skybox <file.ktx2>" << std::endl
			<< " +/- or scroll: step through mip levels" << std::endl
			<< " c : toggle flat-cross display" << std::endl;

		return 1;
	}

	auto* obj = osgDB::readObjectFile(argv[1]);

	if(!obj) {
		std::cerr << "ktx2-skybox: failed to load '" << argv[1] << "'" << std::endl;

		return 1;
	}

	auto* texcm = dynamic_cast<osg::TextureCubeMap*>(obj);

	if(!texcm) {
		std::cerr
			<< "ktx2-skybox: '" << argv[1]
			<< "' is not a TextureCubeMap (got " << obj->className() << ")"
			<< std::endl;

		return 1;
	}

	int maxMip = 0;

	if(auto* img = texcm->getImage(0)) {
		int numMips = static_cast<int>(img->getNumMipmapLevels());
		maxMip = std::max(0, numMips - 1);

		std::cout
			<< "ktx2-skybox: loaded '" << argv[1] << "' "
			<< img->s() << "x" << img->t() << " " << numMips << " mip levels"
			<< std::endl;
	}

	else {
		std::cout
			<< "ktx2-skybox: loaded '" << argv[1] << "' (no image data on face 0)"
			<< std::endl;
	}

	texcm->setUnRefImageDataAfterApply(false);

	auto* mipUniform = new osg::Uniform("mipLevel", 0.0f);

	// ---- Skybox mode ----
	auto* skyGeode = new osg::Geode();

	skyGeode->addDrawable(new osg::ShapeDrawable(new osg::Box(osg::Vec3(), 200.0f)));

	{
		auto* prog = new osg::Program();

		prog->addShader(new osg::Shader(osg::Shader::VERTEX, SKYBOX_VERT));
		prog->addShader(new osg::Shader(osg::Shader::FRAGMENT, SKYBOX_FRAG));

		auto* ss = skyGeode->getOrCreateStateSet();

		ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
		ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
		ss->setMode(GL_TEXTURE_CUBE_MAP_SEAMLESS, osg::StateAttribute::ON);
		ss->setAttributeAndModes(prog, osg::StateAttribute::ON);
		ss->setTextureAttributeAndModes(0, texcm, osg::StateAttribute::ON);
		ss->addUniform(new osg::Uniform("envMap", 0));
		ss->addUniform(mipUniform);
	}

	// ---- Cross mode ----
	auto* crossGeode = new osg::Geode();

	crossGeode->addDrawable(buildCrossGeometry());

	{
		auto* prog = new osg::Program();

		prog->addShader(new osg::Shader(osg::Shader::VERTEX, CROSS_VERT));
		prog->addShader(new osg::Shader(osg::Shader::FRAGMENT, SKYBOX_FRAG));
		prog->addBindAttribLocation("faceDir", 1);

		auto* ss = crossGeode->getOrCreateStateSet();

		ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
		ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
		ss->setMode(GL_TEXTURE_CUBE_MAP_SEAMLESS, osg::StateAttribute::ON);
		ss->setAttributeAndModes(prog, osg::StateAttribute::ON);
		ss->setTextureAttributeAndModes(0, texcm, osg::StateAttribute::ON);
		ss->addUniform(new osg::Uniform("envMap", 0));
		ss->addUniform(mipUniform);
	}

	auto* sw = new osg::Switch();

	sw->addChild(skyGeode, true); // 0 = skybox (default)
	sw->addChild(crossGeode, false); // 1 = cross

	osgViewer::Viewer viewer;

	viewer.setSceneData(sw);
	viewer.addEventHandler(new osgViewer::StatsHandler());
	viewer.addEventHandler(new MipHandler(mipUniform, maxMip));
	viewer.addEventHandler(new ModeHandler(sw));

	std::cout
		<< "Controls: +/- or scroll to step mip levels (0 = sharpest, "
		<< maxMip << " = roughest)" << std::endl
		<< " 'c' to toggle flat-cross display" << std::endl;

	return viewer.run();
}
