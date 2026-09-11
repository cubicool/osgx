#include "osgx/LambertianBake.hpp"
#include "osgx/IBL.hpp"
#include "osgx/RTT.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/GL>
#include <osg/Group>
#include <osg/Notify>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <string>

#ifndef GL_RGB32F
#  define GL_RGB32F 0x8815
#endif
#ifndef GL_COLOR_BUFFER_BIT
#  define GL_COLOR_BUFFER_BIT 0x00004000
#endif

namespace osgx {

namespace {

constexpr const char FULLSCREEN_VERT[] = R"GLSL(
#version 460 core
in vec4 osg_Vertex;
in vec2 osg_MultiTexCoord0;
out vec2 vUV;
void main() {
	vUV = osg_MultiTexCoord0;
	gl_Position = vec4(osg_Vertex.xy, 0.0, 1.0);
}
)GLSL";

constexpr const char LAMBERTIAN_FRAG[] = R"GLSL(
#version 460 core
const float PI = 3.14159265359;
uniform sampler2D equirectTex;
uniform int faceIndex;
uniform int sampleCount;
uniform float fireflyClamp;
in vec2 vUV;
out vec4 fragColor;

// Rescales `color` down (preserving hue) if its luminance exceeds `maxLuminance` -- see
// LambertianBakeOptions::fireflyClamp for why this exists.
vec3 clampFirefly(vec3 color, float maxLuminance) {
	float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));

	return luminance > maxLuminance ? color * (maxLuminance / luminance) : color;
}

float radicalInverseVdC(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);

	return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint count) {
	return vec2(float(i) / float(count), radicalInverseVdC(i));
}

vec3 cubeDirection(int face, vec2 uv) {
	if(face == 0) return normalize(vec3( 1.0, -uv.y, -uv.x));
	if(face == 1) return normalize(vec3(-1.0, -uv.y,  uv.x));
	if(face == 2) return normalize(vec3( uv.x,  1.0,  uv.y));
	if(face == 3) return normalize(vec3( uv.x, -1.0, -uv.y));
	if(face == 4) return normalize(vec3( uv.x, -uv.y,  1.0));

	return normalize(vec3(-uv.x, -uv.y, -1.0));
}

vec3 glToZUp(vec3 direction) {
	return vec3(direction.x, -direction.z, direction.y);
}

vec2 equirectUV(vec3 directionZUp) {
	// The integration basis is Z-up, but the HDR's equirectangular pixels and the output
	// cubemap follow the ordinary KTX/OpenGL convention. Convert at this boundary so serialized
	// Lambertian cubes agree with the GGX baker and can be mixed with external KTX2 cubemaps.
	vec3 direction = vec3(directionZUp.x, directionZUp.z, -directionZUp.y);
	// Cannon_Exterior and Khronos's prefiltered reference use +Z at the panorama's horizontal
	// midpoint. The previous -PI/2 offset rotated every serialized cube by one quarter turn.
	float phi = atan(direction.z, direction.x);
	float theta = acos(clamp(direction.y, -1.0, 1.0));

	return vec2(mod(phi / (2.0 * PI) + 0.5, 1.0), 1.0 - theta / PI);
}

void main() {
	vec3 normal = glToZUp(cubeDirection(faceIndex, vUV * 2.0 - 1.0));
	vec3 up = abs(normal.z) > 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);
	vec3 irradiance = vec3(0.0);

	for(int i = 0; i < sampleCount; i++) {
		vec2 xi = hammersley(uint(i), uint(sampleCount));
		float sinTheta = sqrt(xi.y);
		float cosTheta = sqrt(1.0 - xi.y);
		float phi = 2.0 * PI * xi.x;
		vec3 local = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
		vec3 direction = tangent * local.x + bitangent * local.y + normal * local.z;

		irradiance += clampFirefly(texture(equirectTex, equirectUV(direction)).rgb, fireflyClamp);
	}

	fragColor = vec4(irradiance / float(sampleCount), 1.0);
}
)GLSL";

osg::ref_ptr<osg::Geode> makeFullscreenQuad() {
	auto quad = osg::createTexturedQuadGeometry(
		osg::Vec3(-1.0f, -1.0f, 0.0f),
		osg::Vec3(2.0f, 0.0f, 0.0f),
		osg::Vec3(0.0f, 2.0f, 0.0f)
	);
	auto geode = new osg::Geode();

	geode->addDrawable(quad);

	return geode;
}

osg::ref_ptr<osg::Program> makeProgram() {
	auto program = new osg::Program();

	program->setName("osgx_ibl_lambertianBake");
	program->addShader(new osg::Shader(osg::Shader::VERTEX, FULLSCREEN_VERT));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, LAMBERTIAN_FRAG));

	return program;
}

}

bool LambertianBakeScene::ready() const {
	return completion && completion->done();
}

LambertianBakeScene LambertianBakeScene::create(
	osg::Image* equirectangularHDR,
	const LambertianBakeOptions& options
) {
	LambertianBakeScene scene;

	if(!equirectangularHDR) return scene;

	const int cubeSize = std::max(options.cubeSize, 1);
	const int sampleCount = std::max(options.sampleCount, 1);
	auto sourceTexture = new osg::Texture2D();

	// Full float, not half: a source HDR's peak radiance can exceed half-float's ~65504 max and
	// silently become +Infinity on upload, which then poisons the cosine-weighted hemisphere
	// average for any output texel whose sample set happens to hit it (see GGXPrefilter.cpp for
	// the same fix on the specular path).
	sourceTexture->setInternalFormat(GL_RGB32F);
	sourceTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	sourceTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	sourceTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	sourceTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	sourceTexture->setResizeNonPowerOfTwoHint(false);
	sourceTexture->setImage(equirectangularHDR);

	auto diffuseTexture = new osg::TextureCubeMap();

	diffuseTexture->setDataVariance(osg::Object::DYNAMIC);
	diffuseTexture->setTextureSize(cubeSize, cubeSize);
	diffuseTexture->setInternalFormat(GL_RGB32F);
	diffuseTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	diffuseTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	diffuseTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	diffuseTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	diffuseTexture->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
	diffuseTexture->setUseHardwareMipMapGeneration(false);

	auto root = new osg::Group();
	auto bakeGroup = new osg::Group();
	auto program = makeProgram();
	auto quad = makeFullscreenQuad();
	osg::ref_ptr<osg::Camera> completionCamera;

	// Not RTT::fullscreenQuad() -- that bakes its own fresh Program+quad per call, whereas this bake
	// deliberately shares one `program`/`quad` pair across all six face cameras (a single compile+
	// link, reused by six draws) instead of six independent ones. The raw RTT constructor still
	// removes the PRE_RENDER/FBO/ABSOLUTE_RF/viewport boilerplate; the rest of the fullscreen-quad
	// shape (identity view/proj, NO_CULLING, depth/cull OVERRIDE) is applied manually below, same as
	// fullscreenQuad() would but against the shared program/quad.
	for(int face = 0; face < 6; face++) {
		auto camera = osgx::make_nref<osgx::RTT>(
			"osgx_LambertianBake_f" + std::to_string(face), cubeSize, cubeSize
		);

		camera->setRenderOrder(osg::Camera::PRE_RENDER, face);
		camera->setClearMask(GL_COLOR_BUFFER_BIT);
		camera->setProjectionMatrix(osg::Matrix::identity());
		camera->setViewMatrix(osg::Matrix::identity());
		camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
		camera->setCullingMode(osg::Camera::NO_CULLING);
		camera->attach(
			osg::Camera::COLOR_BUFFER0,
			diffuseTexture,
			0,
			static_cast<unsigned int>(face),
			false
		);
		camera->setUpdateCallback(new RunOnceCallback());

		auto* stateSet = camera->getOrCreateStateSet();

		stateSet->setAttributeAndModes(program, osg::StateAttribute::ON);
		stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
		stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
		stateSet->addUniform(new osg::Uniform("equirectTex", 0));
		stateSet->addUniform(new osg::Uniform("faceIndex", face));
		stateSet->addUniform(new osg::Uniform("sampleCount", sampleCount));
		stateSet->addUniform(new osg::Uniform("fireflyClamp", options.fireflyClamp));
		stateSet->setTextureAttributeAndModes(0, sourceTexture, osg::StateAttribute::ON);
		camera->addChild(quad);
		bakeGroup->addChild(camera);

		if(face == 5) completionCamera = camera;
	}

	auto completion = new BakeCompletion();

	// The face cameras have explicit PRE_RENDER order, so this post-draw callback cannot run until
	// every output face has been populated. Keeping it on a real FBO pass avoids a dummy camera
	// whose callback timing would depend on a renderer accepting an attachment-less target.
	completionCamera->setPostDrawCallback(completion);
	root->addChild(bakeGroup);

	scene.root = root;
	scene.sourceTexture = sourceTexture;
	scene.diffuseTexture = diffuseTexture;
	scene.completion = completion;

	return scene;
}

bool LambertianBakeScene::rebake(osg::Image* equirectangularHDR) {
	if(!root || !sourceTexture || !completion || !equirectangularHDR) return false;

	sourceTexture->setImage(equirectangularHDR);
	completion->reset();
	rearmRunOnceCallbacks(root);

	return true;
}

LambertianCubeReadback::LambertianCubeReadback(
	osg::TextureCubeMap* srcTex,
	BakeCompletion* completion,
	bool sync
):
_srcTex(srcTex),
_completion(completion),
_sync(sync) {
	_result = new osg::TextureCubeMap();
}

void LambertianCubeReadback::operator()(osg::RenderInfo& ri) const {
	if(_done) return;
	if(!_completion || !_completion->done()) return;
	if(!_srcTex) return;

	auto* texObj = _srcTex->getTextureObject(ri.getContextID());

	if(!texObj) {
		OSG_WARN << "osgx::ibl: Lambertian cubemap not on GPU yet; retrying next frame" << std::endl;

		return;
	}

	if(_sync) glFinish();

#if OSG_MIN_VERSION_REQUIRED(3, 7, 0)
	texObj->bind(*ri.getState());
#else
	texObj->bind();
#endif

	readCubeMapFaces(ri.getContextID(), GL_FLOAT, false, _result);

	_done = true;
}

osg::ref_ptr<osg::TextureCubeMap> LambertianCubeReadback::finish() const {
	if(!isDone()) return nullptr;

	osg::TextureCubeMap* result = getResult();

	result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	result->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	result->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	result->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
	result->setUseHardwareMipMapGeneration(false);

	return result;
}

}
