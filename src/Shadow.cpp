#include "ShaderLibs.hpp"

#include "osgx/Array.hpp"
#include "osgx/Library.hpp"
#include "osgx/Shadow.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/GL>
#include <osg/Math>
#include <osg/Matrix>
#include <osg/Matrixd>
#include <osg/Matrixf>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace osgx {

namespace {

// Vertex-transform-only, empty-fragment Program installed on every directional shadow camera's
// own StateSet (ON|OVERRIDE) - see ShadowMap::create()'s own comment for why. Uses OSG's
// standard osg_Vertex/osg_ModelViewProjectionMatrix names (auto-bound by OSG, same as every other
// osgx/pyosg-lighting shader - no explicit addBindAttribLocation() needed) so it works unmodified
// against any subgraph, not just a specific vertex-attribute convention.
constexpr const char DEPTH_ONLY_VERTEX_SHADER[] = R"GLSL(
#version 460 core

in vec4 osg_Vertex;

uniform mat4 osg_ModelViewProjectionMatrix;

void main() {
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
)GLSL";

constexpr const char DEPTH_ONLY_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core

void main() {
}
)GLSL";

osg::ref_ptr<osg::Program> makeDepthOnlyProgram() {
	auto program = osgx::make_nref<osg::Program>("osgx_shadow_DepthOnly");

	program->addShader(new osg::Shader(osg::Shader::VERTEX, DEPTH_ONLY_VERTEX_SHADER));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, DEPTH_ONLY_FRAGMENT_SHADER));

	return program;
}

// Distance-only Program for ShadowMap::createPoint()'s six-camera cube capture (installed via
// CaptureCubeMap::Options::overrideProgram). Unlike DEPTH_ONLY_*_SHADER above (which relies purely
// on hardware NDC depth, never read back), this writes an explicit LINEAR distance to a
// single-channel color output - a point light's shadow test (SHADOW_FACTOR_POINT, Shadow.hpp)
// compares real distances in a cube, not projected NDC depth. Each capture camera's own eye IS the
// light (CaptureCubeMap positions all six there), so "distance to light" is just the
// fragment's own view-space distance from the origin - no separate light-position uniform needed
// here at all, only when SAMPLING the result later.
constexpr const char DISTANCE_ONLY_VERTEX_SHADER[] = R"GLSL(
#version 460 core

in vec4 osg_Vertex;

uniform mat4 osg_ModelViewMatrix;
uniform mat4 osg_ModelViewProjectionMatrix;

out vec3 vViewPos;

void main() {
	vViewPos = (osg_ModelViewMatrix * osg_Vertex).xyz;
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
)GLSL";

constexpr const char DISTANCE_ONLY_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core

in vec3 vViewPos;

out float fragDistance;

void main() {
	fragDistance = length(vViewPos);
}
)GLSL";

osg::ref_ptr<osg::Program> makeDistanceOnlyProgram() {
	auto program = osgx::make_nref<osg::Program>("osgx_shadow_DistanceOnly");

	program->addShader(new osg::Shader(osg::Shader::VERTEX, DISTANCE_ONLY_VERTEX_SHADER));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, DISTANCE_ONLY_FRAGMENT_SHADER));

	return program;
}

// Starting point for ShadowMap::normalOffset, in texels of this map's own footprint - a world-
// space margin proportional to what one texel actually covers, not a fixed literal. 1.5 texels
// is deliberately modest (enough to clear ordinary quantization/facet-slope acne without
// visible peter-panning on the table-test scene); not yet exposed as its own Options field -
// see ai/todo-shadow.md's scale-aware bias item for why.
constexpr float NORMAL_OFFSET_TEXELS = 1.5f;

// Shared by ShadowMap::create()/ShadowMap::reposition() - the only difference
// between "create" and "reposition" is whether a new camera/texture gets allocated around this
// math, not the math itself.
void computeDirectionalShadowMatrices(
	const osg::Vec3& lightDirection,
	const ShadowMap::Coverage& coverage,
	const ShadowMap::Options& options,
	osg::Matrixd& lightView,
	osg::Matrixd& lightProj,
	double& outExtent
) {
	const osg::BoundingSphere bound = coverage.bound();
	osg::Vec3 dir = lightDirection;

	dir.normalize();

	const double extent = options.extent > 0.0f
		? double(options.extent)
		: double(bound.radius()) * double(options.margin);
	const double distance = extent * 2.0;
	const osg::Vec3 lightPos = bound.center() - dir * float(distance);

	// Up hint must not be nearly parallel to dir, or lookAt()'s basis degenerates - the classic
	// failure mode is a light aligned with world-up ((0,0,1) in this Z-up engine), which is why
	// this used to hardcode (0,1,0) here. But (0,1,0) is an ordinary HORIZONTAL direction in a
	// Z-up world, not a safe "other axis": any light with a dominant Y component (a perfectly
	// normal shallow/grazing directional light, not just a Z-aligned one) is then nearly
	// parallel to the hint instead, degenerating the same way - confirmed live via a 25-degree-
	// elevation light (dir.y() ~ -0.91), which is nowhere near a hardcoded "is this basically
	// vertical" cutoff but was already badly degenerate. Compare alignment with BOTH candidates
	// instead of gating on dir.z() alone, and take whichever is less parallel to dir - same
	// spirit as computeSpotShadowMatrices()'s dynamic up below, but a magnitude comparison
	// rather than a fixed threshold, so there is no dangerous middle ground between the two.
	const osg::Vec3 up = std::abs(dir.y()) < std::abs(dir.z())
		? osg::Vec3(0.0, 1.0, 0.0)
		: osg::Vec3(0.0, 0.0, 1.0);

	lightView = osg::Matrix::lookAt(lightPos, bound.center(), up);

	const double near_ = std::max(0.01, distance - extent);
	const double far_ = distance + extent;

	// Orthographic, not perspective - a directional light's rays are parallel by construction;
	// see ShadowMap::Options::extent's own comment for why a perspective frustum here is simply
	// wrong (not a style choice) for this light type.
	lightProj = osg::Matrix::ortho(-extent, extent, -extent, extent, near_, far_);
	outExtent = extent;
}

// Perspective, from the light's own position along its direction: a spot light's rays diverge
// from a point. The field of view covers the outer cone (plus a little, so the cone's own edge
// isn't at the texture border); near/far bracket the scene's bound as seen from the light.
void computeSpotShadowMatrices(
	const osg::Vec3& position,
	const osg::Vec3& direction,
	float outerConeAngle,
	const ShadowMap::Coverage& coverage,
	const ShadowMap::Options& options,
	osg::Matrixd& lightView,
	osg::Matrixd& lightProj,
	double& outDistance
) {
	const osg::BoundingSphere bound = coverage.bound();
	osg::Vec3 dir = direction;

	dir.normalize();

	// An up vector not parallel to the direction (see computeDirectionalShadowMatrices()).
	const osg::Vec3 up = std::abs(dir.y()) < 0.99f ? osg::Vec3(0.0, 1.0, 0.0) : osg::Vec3(1.0, 0.0, 0.0);

	lightView = osg::Matrix::lookAt(position, position + dir, up);

	const double reach = double(bound.radius()) * double(options.margin);
	const double distance = double((bound.center() - position).length());
	const double far_ = distance + reach;
	const double near_ = std::max(far_ * 0.001, distance - reach);
	const double fovy = std::min(
		osg::RadiansToDegrees(2.0 * double(outerConeAngle)) * 1.1,
		170.0
	);

	lightProj = osg::Matrix::perspective(fovy, 1.0, near_, far_);
	outDistance = distance;
}

}

bool ShadowMap::valid() const {
	if(camera.valid() && depthTexture.valid() && shadowMatrix.valid()) return true;

	return cubeCapture.texture.valid() && lightPosition.valid();
}

namespace {

// The depth texture, depth-only camera, and uniforms shared by every ShadowMap kind.
ShadowMap makeShadowMap(
	const char* name,
	const ShadowMap::Options& options,
	const osg::Matrixd& lightView,
	const osg::Matrixd& lightProj,
	float texelWorldSize
) {
	ShadowMap result;

	result.lightView = lightView;
	result.lightProj = lightProj;

	result.depthTexture = osgx::make_ref<osg::Texture2D>();
	result.depthTexture->setTextureSize(options.size, options.size);
	result.depthTexture->setInternalFormat(GL_DEPTH_COMPONENT24);
	result.depthTexture->setSourceFormat(GL_DEPTH_COMPONENT);
	result.depthTexture->setSourceType(GL_FLOAT);
	result.depthTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
	result.depthTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
	result.depthTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	result.depthTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	// Render target (shadow camera) AND sampler input (the lighting pass's osgx_ShadowFactor())
	// - without DYNAMIC, OSG's default StateAttribute caching can treat this as unchanging after
	// its first successful bind and stop correctly re-applying it later. Same fix as
	// GBuffer.cpp's own color/depth textures.
	result.depthTexture->setDataVariance(osg::Object::DYNAMIC);

	// Locally osgx::RTT-typed (constructor + initializer-list attach()) - but ShadowMap::camera
	// itself stays osg::ref_ptr<osg::Camera> (see its own declaration) since it's exposed to the
	// Python bindings and osgx::RTT isn't a registered pybind11 type.
	auto camera = osgx::make_nref<osgx::RTT>(name, options.size, options.size);

	camera->setClearMask(GL_DEPTH_BUFFER_BIT);
	camera->setClearDepth(1.0);
	camera->attach({{osg::Camera::DEPTH_BUFFER, result.depthTexture}});
	// Depth-only: the old hand-rolled Python examples attached a dummy color texture here to work
	// around a since-irrelevant pybind11 binding gap (Camera::setDrawBuffer/setReadBuffer weren't
	// exposed to Python yet) - ordinary C++ calls, no workaround needed.
	camera->setDrawBuffer(GL_NONE);
	camera->setReadBuffer(GL_NONE);
	camera->setViewMatrix(result.lightView);
	camera->setProjectionMatrix(result.lightProj);
	// Without this, OSG's CullVisitor silently reclamps near/far from whatever's visible in the
	// shadow camera's own cull pass (e.g. a large ground-plane caster), diverging from the
	// lightProj baked into `shadowMatrix` above - a structural mismatch between what's written
	// into the depth texture and what osgx_ShadowFactor() compares against, which no bias value
	// can compensate for (unlike CaptureCubeMap's point-light path, which already sets this).
	camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
	// ON|OVERRIDE, no PROTECTED: wins over any Program a child subgraph sets on its OWN StateSet
	// with just ON (the convention every osgx::PBRScene/pyosg-lighting Program uses) --
	// see this function's own header comment for the full rationale.
	camera->getOrCreateStateSet()->setAttributeAndModes(
		makeDepthOnlyProgram(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE
	);

	result.camera = camera;

	result.shadowMatrix = new osg::Uniform("osgx_shadowMatrix", osg::Matrixf::identity());
	result.bias = new osg::Uniform("osgx_shadowBias", options.bias);
	result.normalOffset = new osg::Uniform(
		"osgx_shadowNormalOffset", texelWorldSize * NORMAL_OFFSET_TEXELS
	);
	result.strength = new osg::Uniform("osgx_shadowStrength", options.strength);
	result.casterIndex = new osg::Uniform("osgx_shadowCasterIndex", 0);

	result.updateMatrix();

	return result;
}

}

ShadowMap ShadowMap::create(
	const osg::Vec3& lightDirection,
	const ShadowMap::Coverage& coverage,
	const ShadowMap::Options& options
) {
	osg::Matrixd lightView, lightProj;
	double extent = 0.0;

	computeDirectionalShadowMatrices(lightDirection, coverage, options, lightView, lightProj, extent);

	const float texelWorldSize = float(extent * 2.0 / double(options.size));

	return makeShadowMap(
		"osgx_shadow_DirectionalShadowMap", options, lightView, lightProj, texelWorldSize
	);
}

ShadowMap ShadowMap::create(
	const osg::Vec3& lightDirection,
	const ShadowMap::Coverage& coverage
) {
	return create(lightDirection, coverage, Options{});
}

ShadowMap ShadowMap::createSpot(
	const osg::Vec3& position,
	const osg::Vec3& direction,
	float outerConeAngle,
	const ShadowMap::Coverage& coverage,
	const ShadowMap::Options& options
) {
	osg::Matrixd lightView, lightProj;
	double distance = 0.0;

	computeSpotShadowMatrices(
		position,
		direction,
		outerConeAngle,
		coverage,
		options,
		lightView,
		lightProj,
		distance
	);

	// Perspective, so texel world size grows with distance from the light - unlike the
	// directional ortho case, there's no single constant value; this approximates it at the
	// scene bound itself (not the near/far-padded frustum edge) using the cone's own half-angle,
	// consistent with how computeSpotShadowMatrices() sizes the frustum around that same distance.
	const float texelWorldSize = float(
		2.0 * distance * std::tan(double(outerConeAngle)) / double(options.size)
	);

	return makeShadowMap(
		"osgx_shadow_SpotShadowMap", options, lightView, lightProj, texelWorldSize
	);
}

ShadowMap ShadowMap::createSpot(
	const osg::Vec3& position,
	const osg::Vec3& direction,
	float outerConeAngle,
	const ShadowMap::Coverage& coverage
) {
	return createSpot(position, direction, outerConeAngle, coverage, Options{});
}

void ShadowMap::updateMatrix() {
	if(!shadowMatrix) return;

	// OSG row-vector convention: worldPos * (lightView * lightProj) is the same composition GLSL's
	// osgx_shadowMatrix * vec4(worldPos, 1.0) performs once uploaded - see Shadow.hpp's file-level
	// comment for why this needs no main-camera term (unlike the eye-space hand-rolled examples).
	shadowMatrix->set(osg::Matrixf(lightView * lightProj));
}

void ShadowMap::reposition(
	const osg::Vec3& lightDirection,
	const ShadowMap::Coverage& coverage,
	const ShadowMap::Options& options
) {
	if(!camera) return;

	double extent = 0.0;

	computeDirectionalShadowMatrices(lightDirection, coverage, options, lightView, lightProj, extent);

	camera->setViewMatrix(lightView);
	camera->setProjectionMatrix(lightProj);

	if(normalOffset) {
		normalOffset->set(float(extent * 2.0 / double(options.size)) * NORMAL_OFFSET_TEXELS);
	}

	updateMatrix();
}

void ShadowMap::reposition(
	const osg::Vec3& lightDirection,
	const ShadowMap::Coverage& coverage
) {
	reposition(lightDirection, coverage, Options{});
}

void ShadowMap::repositionSpot(
	const osg::Vec3& position,
	const osg::Vec3& direction,
	float outerConeAngle,
	const ShadowMap::Coverage& coverage,
	const ShadowMap::Options& options
) {
	if(!camera) return;

	double distance = 0.0;

	computeSpotShadowMatrices(
		position,
		direction,
		outerConeAngle,
		coverage,
		options,
		lightView,
		lightProj,
		distance
	);

	camera->setViewMatrix(lightView);
	camera->setProjectionMatrix(lightProj);

	if(normalOffset) {
		normalOffset->set(
			float(2.0 * distance * std::tan(double(outerConeAngle)) / double(options.size))
				* NORMAL_OFFSET_TEXELS
		);
	}

	updateMatrix();
}

void ShadowMap::repositionSpot(
	const osg::Vec3& position,
	const osg::Vec3& direction,
	float outerConeAngle,
	const ShadowMap::Coverage& coverage
) {
	repositionSpot(position, direction, outerConeAngle, coverage, Options{});
}

ShadowMap ShadowMap::createPoint(
	const osg::Vec3& position,
	const ShadowMap::Coverage& coverage,
	int cubeSize,
	const ShadowMap::Options& options
) {
	ShadowMap result;

	const osg::BoundingSphere bound = coverage.bound();
	const double reach = double(bound.radius()) * double(options.margin);
	const double farPlane = double((bound.center() - position).length()) + reach;

	result.casters = osgx::make_nref<osg::Group>("osgx_shadow_PointCasters");

	CaptureCubeMap::Options cubeOptions;

	cubeOptions.cubeSize = std::max(cubeSize, 1);
	// 0.05 fixed, not derived like createSpot()'s near_ - unlike a hardware NDC depth buffer, the
	// stored value here is a real linear distance (DISTANCE_ONLY_FRAGMENT_SHADER above), so it
	// carries none of the near:far precision-compression risk ShadowMap::Options::margin's own
	// comment describes; this near plane only needs to keep the depth TEST correct, not preserve
	// storage precision.
	cubeOptions.nearPlane = 0.05;
	cubeOptions.farPlane = farPlane;
	cubeOptions.format = CaptureCubeMap::Format::Distance;
	cubeOptions.overrideProgram = makeDistanceOnlyProgram();
	cubeOptions.continuous = true;
	// Clears every non-geometry texel to well past the far plane, so an unoccluded direction's
	// comparison in osgx_ShadowFactorPoint() always reads as "no occluder" - the cube-map
	// equivalent of the 2D depth map's cleared-to-1.0 (far) background.
	cubeOptions.clearColor = osg::Vec4(float(farPlane) * 2.0f, 0.0f, 0.0f, 1.0f);

	result.cubeCapture = CaptureCubeMap::create(
		result.casters.get(), osg::Vec3d(position), cubeOptions
	);

	result.bias = new osg::Uniform("osgx_shadowBias", options.bias);
	// Cube face FOV is fixed at 90 degrees (tan(45deg) == 1), so texel world size reduces to
	// 2*distance/cubeSize - no cone-angle term needed, unlike createSpot()'s equivalent.
	result.normalOffset = new osg::Uniform(
		"osgx_shadowNormalOffset",
		float(2.0 * (double((bound.center() - position).length())) / double(cubeOptions.cubeSize))
			* NORMAL_OFFSET_TEXELS
	);
	result.strength = new osg::Uniform("osgx_shadowStrength", options.strength);
	result.casterIndex = new osg::Uniform("osgx_shadowCasterIndex", 0);
	result.lightPosition = new osg::Uniform("osgx_shadowLightPos", position);

	return result;
}

ShadowMap ShadowMap::createPoint(
	const osg::Vec3& position,
	const ShadowMap::Coverage& coverage,
	int cubeSize
) {
	return createPoint(position, coverage, cubeSize, Options{});
}

void ShadowMap::repositionPoint(
	const osg::Vec3& position,
	const ShadowMap::Coverage& coverage,
	const ShadowMap::Options& options
) {
	if(!cubeCapture.recapture(osg::Vec3d(position))) return;

	const osg::BoundingSphere bound = coverage.bound();

	// recapture() only re-aims the six cameras' VIEW matrices (a moved light) - the far plane/clear
	// value also need refreshing here, unlike the 2D map kinds: createPoint()'s far plane was sized
	// for the ORIGINAL position, and a light now moved closer to (or past) the scene than that
	// bound would silently clip real casters out of the capture otherwise.
	const double reach = double(bound.radius()) * double(options.margin);
	const double farPlane = double((bound.center() - position).length()) + reach;
	const osg::Vec4 clearColor(float(farPlane) * 2.0f, 0.0f, 0.0f, 1.0f);

	for(auto& faceCamera: cubeCapture.cameras) {
		if(!faceCamera) continue;

		faceCamera->setProjectionMatrixAsPerspective(90.0, 1.0, 0.05, farPlane);
		faceCamera->setClearColor(clearColor);
	}

	if(lightPosition) lightPosition->set(position);

	if(normalOffset && cubeCapture.texture.valid()) {
		const double distance = (bound.center() - position).length();
		const double cubeSize = std::max(1, cubeCapture.texture->getTextureWidth());

		normalOffset->set(float(2.0 * distance / cubeSize) * NORMAL_OFFSET_TEXELS);
	}
}

void ShadowMap::repositionPoint(
	const osg::Vec3& position,
	const ShadowMap::Coverage& coverage
) {
	repositionPoint(position, coverage, Options{});
}

namespace {

// A directional/spot map has a real `camera`; a point map doesn't (see ShadowMap::valid()'s own
// identical discriminant).
bool isPointShadow(const ShadowMap& map) {
	return !map.camera.valid();
}

// Float offsets into one packed osgx_ShadowData2D struct (SHADOW_2D_STRUCT_FLOATS=20 floats/80
// bytes) - must match SHADOW_UNIFORMS_MULTI's GLSL struct layout comment in Shadow.hpp exactly.
constexpr std::size_t SHADOW_2D_MATRIX_OFFSET = 0; // mat4, 16 floats
constexpr std::size_t SHADOW_2D_BIAS_OFFSET = 16;
constexpr std::size_t SHADOW_2D_NORMAL_OFFSET_OFFSET = 17;
constexpr std::size_t SHADOW_2D_STRENGTH_OFFSET = 18;
constexpr std::size_t SHADOW_2D_CASTER_INDEX_OFFSET = 19;
constexpr std::size_t SHADOW_2D_STRUCT_FLOATS = 20;

// Float offsets into one packed osgx_ShadowDataCube struct (SHADOW_CUBE_STRUCT_FLOATS=8 floats/32
// bytes) - std140 packs `bias` into the last 4 bytes of `lightPos`'s own 16-byte slot (a scalar's
// 4-byte alignment lets it follow a vec3 directly); float index 7 is unused tail padding.
constexpr std::size_t SHADOW_CUBE_LIGHT_POS_OFFSET = 0; // vec3
constexpr std::size_t SHADOW_CUBE_BIAS_OFFSET = 3;
constexpr std::size_t SHADOW_CUBE_NORMAL_OFFSET_OFFSET = 4;
constexpr std::size_t SHADOW_CUBE_STRENGTH_OFFSET = 5;
constexpr std::size_t SHADOW_CUBE_CASTER_INDEX_OFFSET = 6;
constexpr std::size_t SHADOW_CUBE_STRUCT_FLOATS = 8;

// Where the cube block starts in ShadowSet::shadowData - right after every 2D slot.
constexpr std::size_t SHADOW_CUBE_BASE = static_cast<std::size_t>(MAX_SHADOWED_2D) * SHADOW_2D_STRUCT_FLOATS;

// GLSL's `casterIndex` is declared `int` but stored in this float-typed backing array -
// std::bit_cast reinterprets the bit pattern without UB, matching LightSet's identical trick
// (Light.cpp's detail::intBitsToFloat/floatBitsToInt) for its own float-backed int fields.
float intBitsToFloat(int value) { return std::bit_cast<float>(value); }

}

bool ShadowSet::valid() const {
	return shader.valid() && shadowData.valid() && shadowDataBinding.valid();
}

ShadowSet ShadowSet::create() {
	ShadowSet result;

	result.shadowMaps2D = new osg::Uniform(
		osg::Uniform::SAMPLER_2D, "osgx_shadowMaps2D", MAX_SHADOWED_2D
	);
	result.shadowMapsCube = new osg::Uniform(
		osg::Uniform::SAMPLER_CUBE, "osgx_shadowMapsCube", MAX_SHADOWED_CUBE
	);

	const auto totalFloats = SHADOW_CUBE_BASE
		+ static_cast<std::size_t>(MAX_SHADOWED_CUBE) * SHADOW_CUBE_STRUCT_FLOATS;

	result.shadowData = new osgx::FloatArray(totalFloats);

	std::fill(result.shadowData->begin(), result.shadowData->end(), 0.0f);
	result.shadowData->setBufferObject(new osg::UniformBufferObject());

	// Index 0 until apply() resolves the "osgx::shadow" slot - same deferred-resolution pattern as
	// LightSet's own "osgx::light" binding (Light.cpp).
	result.shadowDataBinding = new osg::UniformBufferBinding(
		0, result.shadowData, 0, static_cast<GLsizeiptr>(result.shadowData->getTotalDataSize())
	);

	// Every slot starts unused - osgx_ShadowFactorForLight()'s scan never matches a real
	// osgx_lights[] index against an untouched slot (whose casterIndex would otherwise default to
	// 0, silently "claiming" light 0 before any map was ever add()ed).
	for(int i = 0; i < MAX_SHADOWED_2D; i++) {
		const auto base = static_cast<std::size_t>(i) * SHADOW_2D_STRUCT_FLOATS;

		(*result.shadowData)[base + SHADOW_2D_CASTER_INDEX_OFFSET] = intBitsToFloat(-1);
	}

	for(int i = 0; i < MAX_SHADOWED_CUBE; i++) {
		const auto base = SHADOW_CUBE_BASE + static_cast<std::size_t>(i) * SHADOW_CUBE_STRUCT_FLOATS;

		(*result.shadowData)[base + SHADOW_CUBE_CASTER_INDEX_OFFSET] = intBitsToFloat(-1);
	}

	// Content is identical across every ShadowSet - cachedShader() shares one compiled instance
	// process-wide instead of recompiling the same text per Program (see Shader.hpp's own comment
	// on cachedShader()'s intended use: "a library's own default/no-op shader constants ... that's
	// likely to repeat").
	result.shader = osgx::cachedShader(
		osg::Shader::FRAGMENT, resolveShaderLibs(std::string(SHADOW_FACTOR_HOOK_MULTI))
	);
	result.shader->setName("osgx_ShadowFactorHookMulti");

	return result;
}

void ShadowSet::add(const ShadowMap& map) {
	if(!map.valid()) throw std::invalid_argument("ShadowSet::add(): ShadowMap is not valid");

	if(isPointShadow(map)) {
		if(_nextCube >= MAX_SHADOWED_CUBE) {
			throw std::out_of_range("ShadowSet::add(): no free point (cube) shadow slot");
		}

		_mapsCube[_nextCube++] = &map;
	}

	else {
		if(_next2D >= MAX_SHADOWED_2D) {
			throw std::out_of_range("ShadowSet::add(): no free directional/spot (2D) shadow slot");
		}

		_maps2D[_next2D++] = &map;
	}

	sync();
}

void ShadowSet::sync() {
	for(int slot = 0; slot < _next2D; slot++) {
		const ShadowMap* map = _maps2D[slot];
		const auto base = static_cast<std::size_t>(slot) * SHADOW_2D_STRUCT_FLOATS;
		osg::Matrixf matrix;
		float bias = 0.0f, normalOffset = 0.0f, strength = 0.0f;
		int caster = 0;

		map->shadowMatrix->get(matrix);
		map->bias->get(bias);
		map->normalOffset->get(normalOffset);
		map->strength->get(strength);
		map->casterIndex->get(caster);

		// Raw memcpy, not a per-component loop - matrix.ptr()'s 16 floats are exactly the bytes
		// glUniformMatrix4fv(..., GL_FALSE, ptr) used to upload (see osg::Uniform::setElement's own
		// implementation), which std140's default column_major layout expects verbatim.
		std::copy(
			matrix.ptr(),
			matrix.ptr() + 16,
			shadowData->begin() + static_cast<std::ptrdiff_t>(base + SHADOW_2D_MATRIX_OFFSET)
		);
		(*shadowData)[base + SHADOW_2D_BIAS_OFFSET] = bias;
		(*shadowData)[base + SHADOW_2D_NORMAL_OFFSET_OFFSET] = normalOffset;
		(*shadowData)[base + SHADOW_2D_STRENGTH_OFFSET] = strength;
		(*shadowData)[base + SHADOW_2D_CASTER_INDEX_OFFSET] = intBitsToFloat(caster);
	}

	for(int slot = 0; slot < _nextCube; slot++) {
		const ShadowMap* map = _mapsCube[slot];
		const auto base = SHADOW_CUBE_BASE + static_cast<std::size_t>(slot) * SHADOW_CUBE_STRUCT_FLOATS;
		osg::Vec3 lightPos;
		float bias = 0.0f, normalOffset = 0.0f, strength = 0.0f;
		int caster = 0;

		map->lightPosition->get(lightPos);
		map->bias->get(bias);
		map->normalOffset->get(normalOffset);
		map->strength->get(strength);
		map->casterIndex->get(caster);

		(*shadowData)[base + SHADOW_CUBE_LIGHT_POS_OFFSET + 0] = lightPos.x();
		(*shadowData)[base + SHADOW_CUBE_LIGHT_POS_OFFSET + 1] = lightPos.y();
		(*shadowData)[base + SHADOW_CUBE_LIGHT_POS_OFFSET + 2] = lightPos.z();
		(*shadowData)[base + SHADOW_CUBE_BIAS_OFFSET] = bias;
		(*shadowData)[base + SHADOW_CUBE_NORMAL_OFFSET_OFFSET] = normalOffset;
		(*shadowData)[base + SHADOW_CUBE_STRENGTH_OFFSET] = strength;
		(*shadowData)[base + SHADOW_CUBE_CASTER_INDEX_OFFSET] = intBitsToFloat(caster);
	}

	shadowData->dirty();
}

void ShadowSet::apply(osg::StateSet* stateSet) const {
	auto& bindings = osgx::Library::instance().bindings();

	// Texture binding needs the StateSet, so it happens here rather than in add() - one named
	// slot per array element, matching the single-map design's own "osgx::shadowMap" convention,
	// just N times over, so this can never collide with any other texture the rest of the
	// pipeline reserves.
	for(int slot = 0; slot < _next2D; slot++) {
		const auto unit = bindings.get("osgx::shadowMap2D#" + std::to_string(slot));

		stateSet->setTextureAttributeAndModes(
			unit, _maps2D[slot]->depthTexture, osg::StateAttribute::ON
		);
		shadowMaps2D->setElement(static_cast<unsigned int>(slot), static_cast<int>(unit));
	}

	for(int slot = 0; slot < _nextCube; slot++) {
		const auto unit = bindings.get("osgx::shadowMapCube#" + std::to_string(slot));

		stateSet->setTextureAttributeAndModes(
			unit, _mapsCube[slot]->cubeCapture.texture, osg::StateAttribute::ON
		);
		shadowMapsCube->setElement(static_cast<unsigned int>(slot), static_cast<int>(unit));
	}

	// Any slot beyond what was actually add()ed is provably dead code - osgx_ShadowFactorForLight()
	// only ever reaches a slot whose casterIndex matches a real osgx_lights[] index, and unused
	// slots stay at the -1 sentinel forever (ShadowSet::create()) - so nothing GLSL-side ever
	// samples these. But leaving them at osg::Uniform's zero-initialized default (texture unit 0)
	// is still a real bug: once a Program actually, dynamically samples BOTH a sampler2D AND a
	// samplerCube in the same draw call (the whole point of this redesign), NVIDIA's "program
	// texture usage" validation checks every declared sampler value in the program, reachable or
	// not - and finds the untouched slots of BOTH arrays pointing at unit 0, which is never safe to
	// share between two different sampler types (it's osgx::material.baseColor's own preferred
	// index - see Bindings::unused()'s own comment, Library.hpp, for the full story). One shared
	// unused() unit covers both arrays at once - nothing is ever bound there, so a sampler2D and a
	// samplerCube both pointing at it can't disagree about anything.
	if(_next2D < MAX_SHADOWED_2D || _nextCube < MAX_SHADOWED_CUBE) {
		const auto unit = static_cast<int>(bindings.unused());

		for(int slot = _next2D; slot < MAX_SHADOWED_2D; slot++) {
			shadowMaps2D->setElement(static_cast<unsigned int>(slot), unit);
		}

		for(int slot = _nextCube; slot < MAX_SHADOWED_CUBE; slot++) {
			shadowMapsCube->setElement(static_cast<unsigned int>(slot), unit);
		}
	}

	stateSet->addUniform(shadowMaps2D);
	stateSet->addUniform(shadowMapsCube);

	shadowDataBinding->setIndex(bindings.get("osgx::shadow"));
	stateSet->setAttributeAndModes(shadowDataBinding, osg::StateAttribute::ON);
}

void registerShadowShaderLibs() {
	static const osgx::ShaderLib libs[] = {
		{"SHADOW_FACTOR_DECL", "osgx_ShadowFactorForLight", SHADOW_FACTOR_DECL},
		{"SHADOW_UNIFORMS_MULTI", "osgx_shadowMaps2D", SHADOW_UNIFORMS_MULTI},
		{"SHADOW_FACTOR_2D", "osgx_ShadowFactor2D", SHADOW_FACTOR_2D},
		{"SHADOW_FACTOR_CUBE", "osgx_ShadowFactorCube", SHADOW_FACTOR_CUBE},
	};

	::osgx::registerShaderLibs("osgx::shadow", libs);
}

}
