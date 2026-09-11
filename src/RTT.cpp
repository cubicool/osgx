#include "osgx/RTT.hpp"
#include "osgx/IBL.hpp" // osgx::FULLSCREEN_VERT

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/Geode>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

namespace osgx {

RTT::RTT(int width, int height, osg::Transform::ReferenceFrame referenceFrame) {
	setRenderOrder(osg::Camera::PRE_RENDER);
	setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
	setReferenceFrame(referenceFrame);
	setViewport(0, 0, width, height);
}

RTT::~RTT() {}

void RTT::attach(const AttachmentList& attachments) {
	for(const auto& [component, texture]: attachments) osg::Camera::attach(component, texture);
}

osg::ref_ptr<RTT> RTT::fullscreenQuad(int width, int height, const char* fragmentShaderSrc) {
	auto rtt = osgx::make_ref<RTT>(width, height);

	rtt->setProjectionMatrix(osg::Matrix::identity());
	rtt->setViewMatrix(osg::Matrix::identity());
	rtt->setClearMask(GL_COLOR_BUFFER_BIT);
	rtt->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
	rtt->setCullingMode(osg::Camera::NO_CULLING);

	auto* ss = rtt->getOrCreateStateSet();

	// This quad covers every pixel unconditionally, so it must own its own depth/cull state
	// (OVERRIDE) rather than inherit whatever the surrounding framebuffer happens to be carrying --
	// see this factory's own header comment for the silent-failure mode this avoids.
	ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
	ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

	auto program = osgx::make_ref<osg::Program>();

	program->addShader(new osg::Shader(osg::Shader::VERTEX, osgx::FULLSCREEN_VERT));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, fragmentShaderSrc));

	ss->setAttributeAndModes(program, osg::StateAttribute::ON);

	auto quad = osg::createTexturedQuadGeometry(
		osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0)
	);
	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(quad);
	rtt->addChild(geode);
	rtt->setQuad(quad);

	return rtt;
}

}
