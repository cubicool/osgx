#include "osgx/GGXPrefilter.hpp"
#include "osgx/IBL.hpp"
#include "osgx/RTT.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Notify>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Viewport>

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

static const char* FULLSCREEN_VERT = R"GLSL(
#version 460 core
in vec4 osg_Vertex;
in vec2 osg_MultiTexCoord0;
out vec2 vUV;
void main() {
	vUV = osg_MultiTexCoord0;
	gl_Position = vec4(osg_Vertex.xy, 0.0, 1.0);
}
)GLSL";

static const char* PREFILTER_FRAG = R"GLSL(
#version 460 core
const float PI = 3.14159265359;
uniform sampler2D equirectTex;
uniform int faceIndex;
uniform float roughness;
uniform int equirectWidth;
uniform int equirectHeight;
uniform int sampleCount;
uniform float fireflyClamp;
in vec2 vUV;
out vec4 fragColor;

// Rescales `color` down (preserving hue) if its luminance exceeds `maxLuminance` - see
// GGXPrefilterOptions::fireflyClamp for why this exists.
vec3 clampFirefly(vec3 color, float maxLuminance) {
	float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));

	return luminance > maxLuminance ? color * (maxLuminance / luminance) : color;
}

float RadicalInverse_VdC(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint N) { return vec2(float(i)/float(N), RadicalInverse_VdC(i)); }

vec3 importanceSampleGGX(vec2 Xi, vec3 N, float rough) {
	float a = rough * rough;
	float phi = 2.0 * PI * Xi.x;
	float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
	vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 T = normalize(cross(up, N));
	vec3 B = cross(N, T);
	return normalize(T * H.x + B * H.y + N * H.z);
}

vec3 dir_gl_to_zup(vec3 d) { return vec3(d.x, -d.z, d.y); }

vec2 equirect_uv(vec3 dir_zup) {
	vec3 d = vec3(dir_zup.x, dir_zup.z, -dir_zup.y);
	// Keep the serialized GGX cube in the same Khronos/KTX-facing panorama convention as the
	// Lambertian baker: +Z lies at the panorama's horizontal midpoint.
	float phi = atan(d.z, d.x);
	float theta = acos(clamp(d.y, -1.0, 1.0));
	return vec2(mod(phi / (2.0 * PI) + 0.5, 1.0), 1.0 - theta / PI);
}

void main() {
	uint NUM_SAMPLES = uint(sampleCount);
	vec2 uv = vUV * 2.0 - 1.0;
	vec3 N;
	if(faceIndex == 0) N = normalize(vec3( 1.0, -uv.y, -uv.x));
	else if(faceIndex == 1) N = normalize(vec3(-1.0, -uv.y, uv.x));
	else if(faceIndex == 2) N = normalize(vec3( uv.x, 1.0, uv.y));
	else if(faceIndex == 3) N = normalize(vec3( uv.x, -1.0, -uv.y));
	else if(faceIndex == 4) N = normalize(vec3( uv.x, -uv.y, 1.0));
	else N = normalize(vec3(-uv.x, -uv.y, -1.0));

	vec3 V = N;
	vec3 prefilteredColor = vec3(0.0);
	float totalWeight = 0.0;
	float saTexel = 4.0 * PI / float(equirectWidth * equirectHeight);

	for(uint i = 0u; i < NUM_SAMPLES; i++) {
		vec2 Xi = hammersley(i, NUM_SAMPLES);
		vec3 H = importanceSampleGGX(Xi, N, roughness);
		vec3 L = normalize(2.0 * dot(V, H) * H - V);
		float NdotL = max(dot(N, L), 0.0);
		if(NdotL > 0.0) {
			float NdotH = max(dot(N, H), 0.0);
			float VdotH = max(dot(V, H), 0.0);
			float a = roughness * roughness;
			float a2 = a * a;
			float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
			float D = a2 / (PI * denom * denom);
			float pdf = max(D * NdotH / (4.0 * VdotH), 0.0001);
			float saSample = 1.0 / (float(NUM_SAMPLES) * pdf);
			float mip = roughness == 0.0 ? 0.0 : max(0.5 * log2(saSample / saTexel), 0.0);
			vec2 eqUV = equirect_uv(dir_gl_to_zup(L));
			vec3 sampleColor = clampFirefly(textureLod(equirectTex, eqUV, mip).rgb, fireflyClamp);
			prefilteredColor += sampleColor * NdotL;
			totalWeight += NdotL;
		}
	}
	fragColor = vec4(prefilteredColor / max(totalWeight, 0.001), 1.0);
}
)GLSL";

osg::ref_ptr<osg::Geode> makeFullscreenQuad() {
	auto* quad = osg::createTexturedQuadGeometry(
		osg::Vec3(-1, -1, 0),
		osg::Vec3(2, 0, 0),
		osg::Vec3(0, 2, 0)
	);

	auto* geode = new osg::Geode();

	geode->addDrawable(quad);

	return geode;
}

osg::ref_ptr<osg::Program> makeProgram(const char* vert, const char* frag) {
	auto* p = new osg::Program();

	p->addShader(new osg::Shader(osg::Shader::VERTEX, vert));
	p->addShader(new osg::Shader(osg::Shader::FRAGMENT, frag));

	return p;
}

int mipCountForSize(int size) {
	int count = 0;

	for(int s = size; s >= 1; s >>= 1) count++;

	return std::max(1, count);
}

void updateBakeSourceUniforms(osg::Node* node, int eqW, int eqH) {
	if(!node) return;

	if(auto* ss = node->getStateSet()) {
		if(auto* u = ss->getUniform("equirectWidth")) u->set(eqW);
		if(auto* u = ss->getUniform("equirectHeight")) u->set(eqH);
	}

	if(auto* group = node->asGroup()) {
		for(unsigned int i = 0; i < group->getNumChildren(); i++) {
			updateBakeSourceUniforms(group->getChild(i), eqW, eqH);
		}
	}
}

osg::ref_ptr<osg::TextureCubeMap> makePrefilterBake(
	osg::Texture2D* srcEquirect,
	osg::Group* root,
	int prefilterSize,
	int sampleCount,
	int eqW,
	int eqH,
	float fireflyClamp
) {
	const int numMips = mipCountForSize(prefilterSize);

	auto* prefilterTex = new osg::TextureCubeMap();

	prefilterTex->setDataVariance(osg::Object::DYNAMIC);
	prefilterTex->setTextureSize(prefilterSize, prefilterSize);
	// Full float, not half: a source HDR's peak radiance (e.g. a photographed sun disc) can
	// exceed half-float's ~65504 max and silently become +Infinity on upload/write, which then
	// poisons the importance-sampled weighted average for every texel whose sample cone touches
	// it (see equirectTex/envTex below - this is the same tradeoff, mirrored on the output side).
	prefilterTex->setInternalFormat(GL_RGB32F);
	prefilterTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
	prefilterTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	prefilterTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	prefilterTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	prefilterTex->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
	prefilterTex->setUseHardwareMipMapGeneration(false);
	prefilterTex->setNumMipmapLevels(static_cast<unsigned int>(numMips));

	auto prog = makeProgram(FULLSCREEN_VERT, PREFILTER_FRAG);
	auto quad = makeFullscreenQuad();
	auto* group = new osg::Group();

	for(int mip = 0; mip < numMips; mip++) {
		const float roughness = (numMips > 1) ?
			static_cast<float>(mip) / static_cast<float>(numMips - 1) :
			0.0f
		;
		const int mipSize = std::max(1, prefilterSize >> mip);

		// Not RTT::fullscreenQuad() - see LambertianBake.cpp's matching comment: this bake shares
		// one `prog`/`quad` pair across every mip*face camera (a single compile+link) rather than
		// building a fresh one per call, so the raw RTT constructor is used instead (just the
		// PRE_RENDER/FBO/ABSOLUTE_RF/viewport boilerplate; the rest of the fullscreen-quad shape is
		// applied manually below).
		for(int face = 0; face < 6; face++) {
			auto cam = osgx::make_nref<osgx::RTT>(
				"osgx_GGXPrefilter_m" + std::to_string(mip) + "_f" + std::to_string(face),
				mipSize, mipSize
			);

			cam->setRenderOrder(osg::Camera::PRE_RENDER, 1);
			cam->setClearMask(GL_COLOR_BUFFER_BIT);
			cam->setProjectionMatrix(osg::Matrix::identity());
			cam->setViewMatrix(osg::Matrix::identity());
			cam->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
			cam->setCullingMode(osg::Camera::NO_CULLING);
			cam->setUpdateCallback(new RunOnceCallback());
			cam->attach(
				osg::Camera::COLOR_BUFFER0,
				prefilterTex,
				static_cast<unsigned int>(mip),
				static_cast<unsigned int>(face),
				false
			);

			auto* ss = cam->getOrCreateStateSet();

			ss->setAttributeAndModes(prog);
			ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
			ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
			ss->addUniform(new osg::Uniform("equirectTex", 0));
			ss->addUniform(new osg::Uniform("faceIndex", face));
			ss->addUniform(new osg::Uniform("roughness", roughness));
			ss->addUniform(new osg::Uniform("sampleCount", sampleCount));
			ss->addUniform(new osg::Uniform("fireflyClamp", fireflyClamp));
			ss->addUniform(new osg::Uniform("equirectWidth", eqW));
			ss->addUniform(new osg::Uniform("equirectHeight", eqH));
			ss->setTextureAttributeAndModes(0, srcEquirect, osg::StateAttribute::ON);

			cam->addChild(quad);
			group->addChild(cam);
		}
	}

	root->addChild(group);

	return prefilterTex;
}

}

GGXPrefilterReadback::GGXPrefilterReadback(
	osg::TextureCubeMap* tex,
	int trigger,
	bool syncBeforeRead
):
srcTex(tex),
triggerFrame(trigger),
sync(syncBeforeRead) {
	result = new osg::TextureCubeMap();
}

void GGXPrefilterReadback::reset() {
	frameCount = 0;
	done = false;
	result = new osg::TextureCubeMap();
}

void GGXPrefilterReadback::operator()(osg::RenderInfo& ri) const {
	if(done) return;

	frameCount++;

	if(frameCount < triggerFrame) return;
	if(!srcTex) return;

	auto* texObj = srcTex->getTextureObject(ri.getContextID());

	if(!texObj) {
		OSG_WARN << "osgx::ibl: prefilter texture not on GPU yet; retrying next frame" << std::endl;

		return;
	}

	// The only step here without an osg-level equivalent: forcing the GPU to
	// finish the bake before we trust the texture contents. Skipping this
	// (see GGXPrefilterOptions::syncReadback) is the async/best-effort mode.
	if(sync) glFinish();

#if OSG_MIN_VERSION_REQUIRED(3, 7, 0)
	texObj->bind(*ri.getState());
#else
	texObj->bind();
#endif

	readCubeMapFaces(ri.getContextID(), GL_FLOAT, true, result);

	done = true;
}

GGXPrefilterScene GGXPrefilterScene::create(
	osg::Image* equirectImage,
	const GGXPrefilterOptions& options
) {
	GGXPrefilterScene scene;

	if(!equirectImage) return scene;

	const int prefilterSize = std::max(1, options.prefilterSize);
	const int sampleCount = std::max(1, options.sampleCount);

	auto* envTex = new osg::Texture2D();

	envTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
	envTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	envTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	envTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	// See the matching comment on prefilterTex above - a source HDR's peak radiance can exceed
	// half-float's range, so the source upload needs full float too.
	envTex->setInternalFormat(GL_RGB32F);
	envTex->setResizeNonPowerOfTwoHint(false);
	envTex->setImage(equirectImage);

	auto* root = new osg::Group();
	auto prefilterTex = makePrefilterBake(
		envTex, root, prefilterSize, sampleCount,
		equirectImage->s(),
		equirectImage->t(),
		options.fireflyClamp
	);

	scene.root = root;
	scene.sourceTexture = envTex;
	scene.prefilterTexture = prefilterTex;
	scene.readback = new GGXPrefilterReadback(
		prefilterTex,
		std::max(1, options.readbackFrame),
		options.syncReadback
	);

	return scene;
}

bool GGXPrefilterScene::rebake(osg::Image* equirectImage) {
	if(!root || !sourceTexture || !readback || !equirectImage) {
		return false;
	}

	sourceTexture->setImage(equirectImage);

	updateBakeSourceUniforms(root, equirectImage->s(), equirectImage->t());
	rearmRunOnceCallbacks(root);

	readback->reset();

	return true;
}

osg::ref_ptr<osg::TextureCubeMap> GGXPrefilterReadback::finish() const {
	if(!isDone()) return nullptr;

	osg::TextureCubeMap* finished = getResult();

	finished->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
	finished->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	finished->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	finished->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	finished->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);

	return finished;
}

}
