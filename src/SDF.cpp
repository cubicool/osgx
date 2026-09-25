#include "osgx/SDF.hpp"
#include "osgx/Array.hpp"
#include "osgx/Shader.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/State>

OSGX_ENABLE_WARNINGS

#include <stdexcept>

namespace osgx {

namespace {

// Ported verbatim from osgSlug's Atlas.shaders.cpp (SHADER_LIB_MASK's own "--- Signed distance
// primitives ---" section) - see SDF.hpp's own comment for the extraction history and the two
// renames (Box -> Rect, Pie -> Arc). Hexagon/Octagon/Star are Inigo Quilez's exact SDFs
// (iquilezles.org/articles/distfunctions2d); the rest are the standard closed-form formulas for
// their shapes. Negative = inside, matching every other SDF in this codebase.
constexpr const char* SDF_SHAPES_SRC = R"GLSL(
float osgx_SDF_Circle(vec2 p, vec2 center, float r) {
	return length(p - center) - r;
}

float osgx_SDF_Rect(vec2 p, vec2 center, vec2 halfExtents) {
	vec2 d = abs(p - center) - halfExtents;

	return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0);
}

float osgx_SDF_Capsule(vec2 p, vec2 a, vec2 b, float r) {
	vec2 pa = p - a, ba = b - a;
	float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);

	return length(pa - ba * h) - r;
}

// Filled pie sector. angleStart/angleEnd in radians, standard math convention (0=+X, CCW positive).
float osgx_SDF_Arc(vec2 p, vec2 center, float r, float angleStart, float angleEnd) {
	vec2 q = p - center;
	float midAngle = (angleStart + angleEnd) * 0.5;
	float halfSpan = (angleEnd - angleStart) * 0.5;
	vec2 sc = vec2(sin(halfSpan), cos(halfSpan));
	float cosM = cos(-midAngle), sinM = sin(-midAngle);
	vec2 rp = vec2(q.x * cosM - q.y * sinM, q.x * sinM + q.y * cosM);

	rp.x = abs(rp.x);

	float l = length(rp) - r;
	float m = length(rp - sc * clamp(dot(rp, sc), 0.0, r));

	return max(l, m * sign(sc.y * rp.x - sc.x * rp.y));
}

// Stroked arc (annular band along an arc, not a filled sector). strokeHalfWidth is the band's
// own half-width.
float osgx_SDF_ArcBand(
	vec2 p, vec2 center, float r, float angleStart, float angleEnd, float strokeHalfWidth
) {
	vec2 q = p - center;
	float midAngle = (angleStart + angleEnd) * 0.5;
	float halfSpan = (angleEnd - angleStart) * 0.5;
	float cosM = cos(-midAngle), sinM = sin(-midAngle);
	vec2 rp = vec2(q.x * cosM - q.y * sinM, q.x * sinM + q.y * cosM);

	rp.y = abs(rp.y);

	vec2 sc_x = vec2(cos(halfSpan), sin(halfSpan));
	float k = (sc_x.x * rp.y > sc_x.y * rp.x) ? dot(rp, sc_x) : length(rp);

	return sqrt(max(dot(rp, rp) + r * r - 2.0 * r * k, 0.0)) - strokeHalfWidth;
}

// Rotates p by angle (CCW, radians). Every rotatable shape below pre-rotates its query point by
// -rotation into the shape's own unrotated local frame - same trick for all of them, not worth a
// dedicated per-shape variant.
vec2 osgx_SDF_Rotate(vec2 p, float angle) {
	float c = cos(angle), s = sin(angle);

	return vec2(p.x * c - p.y * s, p.x * s + p.y * c);
}

// Regular hexagon (flat-top at rotation=0). Exact SDF (Inigo Quilez, iquilezles.org/articles/distfunctions2d).
float osgx_SDF_Hexagon(vec2 p, vec2 center, float r, float rotation) {
	vec2 q = abs(osgx_SDF_Rotate(p - center, -rotation));
	const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);

	q -= 2.0 * min(dot(k.xy, q), 0.0) * k.xy;
	q -= vec2(clamp(q.x, -k.z * r, k.z * r), r);

	return length(q) * sign(q.y);
}

// Regular octagon. Exact SDF (Inigo Quilez, iquilezles.org/articles/distfunctions2d).
float osgx_SDF_Octagon(vec2 p, vec2 center, float r, float rotation) {
	vec2 q = abs(osgx_SDF_Rotate(p - center, -rotation));
	const vec3 k = vec3(-0.9238795325, 0.3826834323, 0.4142135623);

	q -= 2.0 * min(dot(vec2(k.x, k.y), q), 0.0) * vec2(k.x, k.y);
	q -= 2.0 * min(dot(vec2(-k.x, k.y), q), 0.0) * vec2(-k.x, k.y);
	q -= vec2(clamp(q.x, -k.z * r, k.z * r), r);

	return length(q) * sign(q.y);
}

// General n-pointed star (Inigo Quilez, iquilezles.org/articles/distfunctions2d). r = outer
// radius, points = point count (rounded to the nearest integer >= 3), innerRatio in [0,1] maps
// to IQ's "m" shape parameter (0 = sharpest spikes, 1 = regular n-gon).
float osgx_SDF_Star(
	vec2 p, vec2 center, float r, float points, float innerRatio, float rotation
) {
	vec2 q = osgx_SDF_Rotate(p - center, -rotation);
	float n = max(round(points), 3.0);
	float m = mix(2.0, n, clamp(innerRatio, 0.0, 1.0));

	float an = 3.14159265 / n;
	float en = 3.14159265 / m;
	vec2 acs = vec2(cos(an), sin(an));
	vec2 ecs = vec2(cos(en), sin(en));

	float bn = mod(atan(q.x, q.y), 2.0 * an) - an;

	q = length(q) * vec2(cos(bn), abs(sin(bn)));
	q -= r * acs;
	q += ecs * clamp(-dot(q, ecs), 0.0, r * acs.y / ecs.y);

	return length(q) * sign(q.x);
}
)GLSL";

// Pure reconstruction helpers for baked distance fields - see SDF.hpp for the contracts. The
// screenPixelRange technique is Viktor Chlumsky's (msdfgen's README); it works identically for a
// single-channel SDF, which is why nothing here cares which type `d` came from.
constexpr const char* SDF_SAMPLING_SRC = R"GLSL(
float osgx_SDF_Median(vec3 msd) {
	return max(min(msd.r, msd.g), min(max(msd.r, msd.g), msd.b));
}

float osgx_SDF_ScreenPixelRange(vec2 uv, vec2 texSize, float pixelRange) {
	vec2 unitRange = vec2(pixelRange) / texSize;
	vec2 screenTexSize = vec2(1.0) / max(fwidth(uv), vec2(1e-8));

	return max(0.5 * dot(unitRange, screenTexSize), 1.0);
}

float osgx_SDF_CoverageFromDistance(float d, float screenPixelRange) {
	return clamp((d - 0.5) * screenPixelRange + 0.5, 0.0, 1.0);
}
)GLSL";

// Declaration-only twin of SDF_SAMPLING_SRC, for a second shader object in the SAME Program that
// needs to CALL these but must not re-define them (GLSL rejects one function defined twice across a
// Program's shader objects). Same idea as PBR's *_DECL entries; exactly one object pulls in
// SAMPLING, every other one pulls in SAMPLING_DECL.
constexpr const char* SDF_SAMPLING_DECL_SRC = R"GLSL(
float osgx_SDF_Median(vec3 msd);
float osgx_SDF_ScreenPixelRange(vec2 uv, vec2 texSize, float pixelRange);
float osgx_SDF_CoverageFromDistance(float d, float screenPixelRange);
)GLSL";

// Must match SDF::_write()'s layout: 8 floats, std430 (vec4 + 4 scalars, no implicit padding).
// `sdfType` is a float (0 = SDF, 1 = MSDF) purely to keep the backing store a single FloatArray, the
// same trick Material's has*Map flags use.
constexpr const char* SDF_TEXTURE_SRC = R"GLSL(
layout(binding = @osgx::sdf.texture@) uniform sampler2D osgx_sdfTexture;

layout(std430, binding = @osgx::sdf@) readonly buffer osgx_SDFBuffer {
	vec4 uvRect;
	float pixelRange;
	float sdfType;
	float pad0;
	float pad1;
} osgx_sdf;

float osgx_SDF_Coverage(vec2 uv) {
	vec2 texUV = mix(osgx_sdf.uvRect.xy, osgx_sdf.uvRect.zw, uv);
	vec4 texel = texture(osgx_sdfTexture, texUV);
	float d = osgx_sdf.sdfType < 0.5 ? texel.r : osgx_SDF_Median(texel.rgb);
	float spr = osgx_SDF_ScreenPixelRange(texUV, vec2(textureSize(osgx_sdfTexture, 0)), osgx_sdf.pixelRange);

	return osgx_SDF_CoverageFromDistance(d, spr);
}
)GLSL";

}

void registerSDFShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"SHAPES", "osgx_SDF_Circle", SDF_SHAPES_SRC},
		{"SAMPLING", "osgx_SDF_Median", SDF_SAMPLING_SRC},
		{"SAMPLING_DECL", "osgx_SDF_MedianDecl", SDF_SAMPLING_DECL_SRC},
		{"TEXTURE", "osgx_SDF_Coverage", SDF_TEXTURE_SRC}
	};

	registerShaderLibs("osgx::sdf", libs);
}

SDF::SDF() {
	registerSDFShaderLibs();
	_initBuffer();
}

SDF::SDF(const SDF& sdf, const osg::CopyOp& copyop):
osg::StateAttribute(sdf, copyop),
_texture(static_cast<osg::Texture2D*>(copyop(sdf._texture.get()))),
_sdfType(sdf._sdfType),
_pixelRange(sdf._pixelRange),
_uvRect(sdf._uvRect) {
	_initBuffer();
}

SDF::~SDF() {}

// Built once (not per-write) so every setter can mutate it in place via dirty() instead of
// standing up a new osg::ShaderStorageBufferObject/GL buffer each call - same as Material.
void SDF::_initBuffer() {
	_buffer = osgx::make_ref<osgx::FloatArray>(static_cast<std::size_t>(8));
	_buffer->setBufferObject(new osg::ShaderStorageBufferObject());

	// Index 0 until apply() resolves the "osgx::sdf" slot.
	_binding = new osg::ShaderStorageBufferBinding(
		0, _buffer, 0, static_cast<GLsizeiptr>(_buffer->getTotalDataSize())
	);

	_write();
}

// Layout must match SDF_TEXTURE_SRC's osgx_SDFBuffer block exactly.
void SDF::_write() {
	// [6], [7]: trailing padding, left at 0.
	_buffer->set({
		_uvRect.x(), _uvRect.y(), _uvRect.z(), _uvRect.w(),
		_pixelRange,
		_sdfType == SDFType::MSDF ? 1.0f : 0.0f
	});

	_buffer->dirty();
}

int SDF::compare(const osg::StateAttribute& sa) const {
	COMPARE_StateAttribute_Types(SDF, sa)

	COMPARE_StateAttribute_Parameter(_texture)
	COMPARE_StateAttribute_Parameter(_sdfType)
	COMPARE_StateAttribute_Parameter(_pixelRange)
	COMPARE_StateAttribute_Parameter(_uvRect)

	return 0;
}

// Read-only over this object's state, and binds its texture directly - see Material::apply()
// (PBR.cpp) for why both matter.
void SDF::apply(osg::State& state) const {
	resolveBinding(_bindingResolved, _binding.get(), "osgx::sdf");

	std::call_once(_unitResolved, [this]() {
		_unit = Library::instance().bindings().get("osgx::sdf.texture");
	});

	if(_texture.valid()) {
		state.setActiveTextureUnit(_unit);

		_texture->apply(state);
	}

	state.applyAttribute(_binding.get());
}

osg::ref_ptr<osg::Texture2D> SDF::makeTexture(osg::Image* image) {
	if(!image) return nullptr;

	if(image->getPixelFormat() == GL_LUMINANCE) {
		image->setPixelFormat(GL_RED);
		image->setInternalTextureFormat(GL_R8);
	}

	osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);

	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

	// A tile atlas is almost never a power of two, and OSG's default is to RESCALE such an image up
	// to one (with a "resizing" warning) - which silently shifts every tile's rect/uvRect and smears
	// the distance field it holds.
	texture->setResizeNonPowerOfTwoHint(false);

	return texture;
}

void SDF::setTexture(osg::Texture2D* texture) {
	_texture = texture;
}

void SDF::setSDFType(SDFType sdfType) {
	_sdfType = sdfType;

	_write();
}

void SDF::setPixelRange(float pixelRange) {
	_pixelRange = pixelRange;

	_write();
}

void SDF::setUVRect(const osg::Vec4& uvRect) {
	_uvRect = uvRect;

	_write();
}

}
