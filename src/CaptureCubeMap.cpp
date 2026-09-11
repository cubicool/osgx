#include "osgx/CaptureCubeMap.hpp"
#include "osgx/RTT.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <string>

#ifndef GL_RGB16F
#  define GL_RGB16F 0x881B
#endif
#ifndef GL_COLOR_BUFFER_BIT
#  define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#  define GL_DEPTH_BUFFER_BIT 0x00000100
#endif

namespace osgx {

namespace {

struct FaceOrientation {
	osg::Vec3d direction;
	osg::Vec3d up;
};

// These directions/up vectors reproduce OpenGL's six cubemap-face orientations. They agree with
// cubeFaceDirection() and a samplerCube's conventional lookup basis.
const FaceOrientation FACE_ORIENTATIONS[] = {
	{{ 1.0,  0.0,  0.0}, { 0.0, -1.0,  0.0}}, // +X
	{{-1.0,  0.0,  0.0}, { 0.0, -1.0,  0.0}}, // -X
	{{ 0.0,  1.0,  0.0}, { 0.0,  0.0,  1.0}}, // +Y
	{{ 0.0, -1.0,  0.0}, { 0.0,  0.0, -1.0}}, // -Y
	{{ 0.0,  0.0,  1.0}, { 0.0, -1.0,  0.0}}, // +Z
	{{ 0.0,  0.0, -1.0}, { 0.0, -1.0,  0.0}}  // -Z
};

void setCaptureView(osg::Camera& camera, const osg::Vec3d& position, unsigned int face) {
	const FaceOrientation& orientation = FACE_ORIENTATIONS[face];

	camera.setViewMatrixAsLookAt(position, position + orientation.direction, orientation.up);
}

}

bool CaptureCubeMapScene::ready() const {
	return completion && completion->done();
}

CaptureCubeMapScene CaptureCubeMapScene::create(
	osg::Node* capturedNode,
	const osg::Vec3d& position,
	const CaptureCubeMapOptions& options
) {
	CaptureCubeMapScene scene;

	if(!capturedNode) return scene;

	const int cubeSize = std::max(options.cubeSize, 1);
	const double nearPlane = std::max(options.nearPlane, 0.0001);
	const double farPlane = std::max(options.farPlane, nearPlane + 0.0001);
	auto texture = new osg::TextureCubeMap();
	auto root = new osg::Group();
	auto completion = new BakeCompletion();

	texture->setDataVariance(osg::Object::DYNAMIC);
	texture->setTextureSize(cubeSize, cubeSize);
	texture->setInternalFormat(GL_RGB16F);
	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	texture->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
	texture->setUseHardwareMipMapGeneration(false);

	for(unsigned int face = 0; face < scene.cameras.size(); face++) {
		auto camera = osgx::make_nref<osgx::RTT>(
			"osgx_CaptureCubeMap_f" + std::to_string(face), cubeSize, cubeSize
		);

		camera->setRenderOrder(osg::Camera::PRE_RENDER, static_cast<int>(face));
		camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		camera->setClearColor(options.clearColor);
		camera->setProjectionMatrixAsPerspective(90.0, 1.0, nearPlane, farPlane);
		camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
		camera->attach(osg::Camera::COLOR_BUFFER0, texture, 0, face, false);
		camera->setUpdateCallback(new RunOnceCallback(false));
		setCaptureView(*camera, position, face);
		camera->addChild(capturedNode);

		if(face == scene.cameras.size() - 1) camera->setPostDrawCallback(completion);

		scene.cameras[face] = camera;
		root->addChild(camera);
	}

	scene.root = root;
	scene.radianceTexture = texture;
	scene.completion = completion;

	return scene;
}

bool CaptureCubeMapScene::recapture(const osg::Vec3d& position) {
	if(!root || !completion) return false;

	for(unsigned int face = 0; face < cameras.size(); face++) {
		if(!cameras[face]) return false;

		setCaptureView(*cameras[face], position, face);
	}

	completion->reset();
	rearmRunOnceCallbacks(root);

	return true;
}

}
