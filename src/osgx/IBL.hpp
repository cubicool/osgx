#pragma once

#include "Shader.hpp"
#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/GL>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>
#include <osgDB/ReadFile>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace osgx {

// Disables a node after its update callback has fired exactly once - e.g. a PRE_RENDER bake
// camera that should render one frame at startup and then go idle. Call rebake() to re-arm it
// (render one more frame - e.g. after swapping the bake's source data).
class RunOnceCallback: public osg::NodeCallback {
public:
	explicit RunOnceCallback(bool traverseChildren=true): _traverseChildren(traverseChildren) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;
	void rebake(osg::Node* node);

private:
	bool _done = false;
	bool _traverseChildren = true;
};

// Re-arms every RunOnceCallback below `node`, including callbacks on nodes currently masked out
// after a prior bake or capture.
void rearmRunOnceCallbacks(osg::Node* node);

// Signals that the final ordered pass in a frame-driven bake has completed. This means the result
// is ready for later GPU sampling in that render context; CPU readback remains a separate,
// explicitly synchronizing operation.
class BakeCompletion: public osg::Camera::DrawCallback {
public:
	void operator()(osg::RenderInfo&) const override {
		_done.store(true, std::memory_order_release);
	}

	bool done() const {
		return _done.load(std::memory_order_acquire);
	}

	void reset() {
		_done.store(false, std::memory_order_release);
	}

private:
	mutable std::atomic<bool> _done{false};
};

// Fullscreen NDC-quad vertex shader - shared by any single-pass bake (BRDF LUT today; future
// bakes that need a rasterized pass can reuse it too).
//
// Kept as an `inline constexpr` header definition (not moved to IBL.cpp like the rest of this
// file): it's bound directly by name in ext/osgx-python.cpp (osgx::FULLSCREEN_VERT), which
// needs real external linkage, AND referenced inside registerIBLShaderLibs()'s `static constexpr
// ShaderLib` array below, which needs a genuine constant expression - `inline constexpr` in a
// header is the one form that satisfies both at once.
inline constexpr const char* FULLSCREEN_VERT = R"GLSL(
#version 430 core
in vec4 osg_Vertex;
in vec2 osg_MultiTexCoord0;
out vec2 vUV;
void main() {
	vUV = osg_MultiTexCoord0;
	gl_Position = vec4(osg_Vertex.xy, 0.0, 1.0);
}
)GLSL";

// Split-sum BRDF LUT bake (Karis 2013) - environment-independent, so it only ever needs to
// bake once. R channel = Fresnel scale, G channel = Fresnel bias; sampled in the consuming
// shader as texture(brdfLUT, vec2(NdotV, roughness)).rg.
inline constexpr const char* BRDF_LUT_FRAG = R"GLSL(
#version 430 core
const float PI = 3.14159265359;
in vec2 vUV;
out vec4 fragColor;

float radicalInverseVdC(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint N) {
	return vec2(float(i) / float(N), radicalInverseVdC(i));
}

vec3 importanceSampleGGX(vec2 Xi, float roughness) {
	float a = roughness * roughness;
	float phi = 2.0 * PI * Xi.x;
	float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	return vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// Correlated Smith visibility, written exactly as the validated Python reference viewer's
// `smith(nv, nl, r)`. This is the visibility term already divided by 4*NdotL*NdotV for the
// split-sum integral; it is not interchangeable with a pair of Schlick G terms.
float smithVisibility(float NdotV, float NdotL, float roughness) {
	float alpha2 = pow(roughness, 4.0);
	float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2);
	float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2);

	return 0.5 / (gv + gl);
}

void main() {
	float NdotV = max(vUV.x, 1e-4);
	float roughness = vUV.y;
	vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);

	float scale = 0.0, bias = 0.0;
	const uint SAMPLES = 512u;

	for(uint i = 0u; i < SAMPLES; i++) {
		vec2 Xi = hammersley(i, SAMPLES);
		vec3 H = importanceSampleGGX(Xi, roughness);
		vec3 L = normalize(2.0 * dot(V, H) * H - V);
		float NdotL = max(L.z, 0.0);
		float NdotH = max(H.z, 0.0);
		float VdotH = max(dot(V, H), 0.0);

		if(NdotL > 0.0) {
			float G_vis = smithVisibility(NdotV, NdotL, roughness) * VdotH * NdotL / NdotH;
			float Fc = pow(1.0 - VdotH, 5.0);

			scale += (1.0 - Fc) * G_vis;
			bias += Fc * G_vis;
		}
	}

	fragColor = vec4(4.0 * scale / float(SAMPLES), 4.0 * bias / float(SAMPLES), 0.0, 1.0);
}
)GLSL";

// Loads a pre-baked GGX-prefiltered cubemap from a .ktx2 file (see
// plugins/ktx2/ReaderWriterKTX2.cpp for the plugin that makes this format readable - must be
// registered with osgDB, same as any other reader/writer plugin). The KTX2 is expected to carry its own
// hand-baked mip chain, one level per roughness step - hardware mipmap generation is disabled
// so OSG doesn't overwrite it.
//
// Returns nullptr (and logs via OSG_WARN) if the path doesn't load, or doesn't load as a
// TextureCubeMap.
osg::ref_ptr<osg::TextureCubeMap> loadPrefilterCubemap(const std::string& path);

// Creates a PRE_RENDER FBO camera that bakes the split-sum BRDF LUT into `lut` exactly once
// (via RunOnceCallback - installed as the camera's update callback). `lut` is configured
// in-place (size/format/filters), matching the out-param convention used by the two-argument
// makePickCamera() overload above. The caller is responsible for:
//
// - adding the returned camera as a child of the scene graph (anywhere - it's ABSOLUTE_RF)
// - NOT expecting it to re-bake on its own: the LUT's only inputs are NdotV and roughness,
//   both baked into the UV axes, so a static environment never needs a second bake. Call
//   rebake() on the camera's RunOnceCallback (via getUpdateCallback()) if that ever changes.
osg::ref_ptr<osg::Camera> makeBRDFLUTCamera(int lutSize, osg::Texture2D* lut);

// One `SharedBRDFLUT::create(lutSize)` call's worth of result: `texture` is always the shared LUT
// for that size. `camera` is only non-null the FIRST time a given `lutSize` is requested in this
// process - that caller is responsible for adding it to a rendered scene graph so the one-time
// bake actually runs. Every later call at the same `lutSize` gets the same `texture` back with
// `camera` left null, because there is nothing left to bake.
struct SharedBRDFLUT {
	osg::ref_ptr<osg::Texture2D> texture;
	osg::ref_ptr<osg::Camera> camera;

	// The split-sum BRDF LUT is a property of THIS CODE - this GGX distribution, this Smith
	// visibility term, this sample count - not of any HDR image, environment, or probe (see the
	// derivation note above makeBRDFLUTCamera()). Every environment requesting the same `lutSize`
	// bakes to byte-identical output, so this bakes it once per size, process-wide, and hands the
	// same GPU texture to every caller after that - never re-derive it per HDR/environment/probe,
	// and never key this cache by anything but `lutSize`. NOTE: unlike every other `::create()`
	// factory in osgx, this one may hand back an ALREADY-EXISTING shared texture rather than a
	// fresh bake - see the comment above for the exact contract.
	static SharedBRDFLUT create(int lutSize);
};

// A CPU-readback companion for makeBRDFLUTCamera()/SharedBRDFLUT::create(), exactly like
// GGXPrefilterReadback/LambertianCubeReadback but simpler: the LUT bakes in exactly one PRE_RENDER
// pass (gated by its own RunOnceCallback), so this only needs a frame-count trigger, not an
// external completion signal. Exists purely for the offline serialize-to-KTX2 use case --
// SharedBRDFLUT::create()'s live consumers never construct one of these.
//
// The result is a bare osg::Image, not a Texture2D: the KTX2 writer plugin has no Texture2D
// support at all (only TextureCubeMap and osg::Image - see plugins/ktx2/ReaderWriterKTX2.cpp), so
// there is nothing gained by wrapping this back into a texture before writing it out.
class BRDFLUTReadback: public osg::Camera::DrawCallback {
public:
	BRDFLUTReadback(osg::Texture2D* srcTex, int triggerFrame=1, bool sync=true):
	_srcTex(srcTex),
	_triggerFrame(triggerFrame),
	_sync(sync) {}

	void operator()(osg::RenderInfo& ri) const override;

	bool isDone() const { return _done; }
	osg::Image* getResult() const { return _result; }

private:
	osg::ref_ptr<osg::Texture2D> _srcTex;
	int _triggerFrame;
	bool _sync;
	mutable int _frameCount = 0;
	mutable osg::ref_ptr<osg::Image> _result;
	mutable bool _done = false;
};

// Reads all six faces of `srcTex` (already bound by the caller) back from the GPU into CPU-side
// osg::Image data on `result`, so it becomes writable by the KTX2 plugin (which reads per-face
// `getImage()` data - see plugins/ktx2/ReaderWriterKTX2.cpp - not a live GL texture). Shared by
// every cubemap readback callback (GGXPrefilterReadback, LambertianCubeReadback); each keeps its
// own trigger condition (frame-count heuristic vs. an exact BakeCompletion signal), since that part
// genuinely differs and isn't worth unifying behind a virtual hook.
void readCubeMapFaces(
	unsigned int contextID,
	GLenum type,
	bool copyMipMapsIfAvailable,
	osg::TextureCubeMap* result
);

// ------------------------------------------------------------------------------------------------
// SH-9 diffuse irradiance
//
// L0-L2 spherical harmonics: 9 RGB coefficients standing in for the whole low-frequency diffuse
// environment - much cheaper than sampling a cubemap per-pixel for diffuse, at the cost of only
// capturing broad/blurry lighting (which is all diffuse irradiance ever needs). Ported from
// 09-ibl.py's compute_sh() (projection) and sh_irradiance() (GLSL evaluation).
// ------------------------------------------------------------------------------------------------

struct SH9 {
	osg::Vec3f coeffs[9];
};

// Projects an equirectangular (2:1) HDR/LDR environment image onto SH9. Cosine-lobe A_l weights
// are baked in here so the GLSL evaluation (SH_IRRADIANCE below) is a plain dot-product sum.
//
// O(width*height) - meant to run once at startup (or once per environment swap), not per frame.
// img's pixel format is read via osg::Image::getColor(), which returns true (unnormalized) float
// radiance for float-format images - use a genuinely HDR-loaded osg::Image (e.g. a .hdr file via
// osgDB::readImageFile()), not an LDR-clamped one, or the diffuse term will be dim/wrong.
SH9 computeSH(const osg::Image* img);

// GLSL evaluation of an SH9 environment at world-space normal N. shCoeffs is a 9-element array
// uniform (or local) - caller declares and binds it under whatever name fits their shader
// (e.g. `uniform vec3 iblSH[9];`), then calls osgx_SHIrradiance(N, iblSH).
//
// Each shCoeffs[i] is A_l * L_lm (the cosine-lobe weight A_l already baked in by computeSH()'s
// projection - see its own comment). Reconstructing irradiance requires multiplying back in the
// SAME per-band Y_lm(n) normalization constant used during projection (0.282095/0.488603/
// 1.092548/0.315392/0.546274 below) - omitting them (as a prior revision of this function did)
// isn't "SH9 just being low-frequency," it's a real scale bug: band 0 alone comes out ~3.5x too
// bright without its 0.282095 factor. Matches OpenSceneGraph.py/pyosg-lighting/09-ibl.py's own
// sh_irradiance() constant-for-constant.
inline constexpr const char* SH_IRRADIANCE = R"GLSL(
vec3 osgx_SHIrradiance(vec3 N, vec3 shCoeffs[9]) {
	return max(
		shCoeffs[0] * 0.282095
		+ shCoeffs[1] * (0.488603 * N.y) + shCoeffs[2] * (0.488603 * N.z) + shCoeffs[3] * (0.488603 * N.x)
		+ shCoeffs[4] * (1.092548 * N.x * N.y) + shCoeffs[5] * (1.092548 * N.y * N.z)
		+ shCoeffs[6] * (0.315392 * (3.0 * N.z * N.z - 1.0))
		+ shCoeffs[7] * (1.092548 * N.x * N.z) + shCoeffs[8] * (0.546274 * (N.x * N.x - N.y * N.y)),
		vec3(0.0)
	);
}
)GLSL";

// ------------------------------------------------------------------------------------------------
// Baked Lambertian (cosine-weighted Monte Carlo) diffuse irradiance cubemap
//
// An alternative to SH9 above: a real per-texel convolution instead of 9 coefficients, so it
// keeps directional detail SH9's low-frequency basis can wash out in high-contrast environments.
// This is what the official Khronos glTF-Sample-Viewer itself bakes for diffuse IBL. Ported
// directly from OpenSceneGraph.py/examples/pyosg-khronos-viewer.py's make_lambertian_environment()
// - same Hammersley/radical-inverse sequence, same per-face tangent-frame construction, same
// GL-cube-face -> Z-up-world axis convention IBL_SPECULAR's own R_gl swap already uses (so
// LAMBERTIAN_IRRADIANCE below applies the identical remap before sampling). Confirmed
// pixel-parity against github.khronos.org/glTF-Sample-Viewer-Release/ via that Python viewer,
// 2026-07-22 - this is a mechanical C++ port of an already-validated algorithm, not a new design.
//
// Lives alongside SH9, not in place of it - SH9 stays the right call for "cheap ambient, zero
// fuss"; reach for this when matching a real reference renderer (or high-contrast environments)
// actually matters. osgGLTF's optional PBR renderer uses this exclusively for Khronos parity.
//
// The tangent-frame/equirect-sampling helpers this relies on (radicalInverseVdC, cubeFaceDirection,
// EquirectView, sampleEquirect) are private to computeLambertianCubeMap()'s implementation and live
// entirely in IBL.cpp - nothing outside this repo's own build has ever referenced them by name.
// ------------------------------------------------------------------------------------------------

// O(6 * size * size * samples) bilinear HDR samples - meant to run once at startup (or once per
// environment swap), same contract as computeSH(). `hdrImg` must be a real HDR-loaded osg::Image
// (not LDR-clamped), same requirement as computeSH().
osg::ref_ptr<osg::TextureCubeMap> computeLambertianCubeMap(
	const osg::Image* hdrImg,
	int size = 64,
	int samples = 256
);

// GLSL evaluation of a baked Lambertian cubemap at world-space normal N - applies the same
// GL-cube-face axis remap IBL_SPECULAR's R_gl uses, so `diffuseEnv` must be sampled with a plain
// `samplerCube` bound to a cubemap baked by computeLambertianCubeMap() above (or an equivalent
// Y-up-convention bake).
inline constexpr const char* LAMBERTIAN_IRRADIANCE = R"GLSL(
vec3 osgx_LambertianIrradiance(vec3 N, samplerCube diffuseEnv) {
	vec3 N_gl = vec3(N.x, N.z, -N.y);

	return texture(diffuseEnv, N_gl).rgb;
}
)GLSL";

// Flat two-color "sky above / ground below" ambient term - no cubemap, BRDF LUT, or SH bake
// needed, so a shader can have *some* ambient response with zero asset loading. This is the
// fallback path 09-ibl.py's evaluateIBL() takes when iblEnabled == 0; pulled out standalone here
// since a quick REPL/demo shader frequently wants exactly this and nothing else - reach for the
// real SH_IRRADIANCE/IBL_SPECULAR pair above once an actual environment is worth loading.
inline constexpr const char* HEMISPHERE_AMBIENT = R"GLSL(
vec3 osgx_HemisphereAmbient(vec3 N, vec3 up, vec3 albedo, float ao, vec3 skyColor, vec3 groundColor) {
	float hemi = dot(N, up) * 0.5 + 0.5;

	return mix(groundColor, skyColor, hemi) * albedo * ao;
}
)GLSL";

// The full diffuse+specular IBL environment inputs osgx_EvaluateIBL() (below) needs: a baked
// Lambertian diffuse cubemap, a GGX-prefiltered specular cubemap + BRDF LUT, independent
// diffuse/specular intensity scalars, and the cubemap lookup basis. Split into its own snippet,
// separate from osgx_EvaluateIBL() itself, the same shape DEFERRED_LIGHTING_INPUTS/GET_GBUFFER
// (osgx::gltf) use - a caller that wants these uniforms declared without osgx_EvaluateIBL()
// (unlikely today, but no reason to force the two together) can request just this one.
//
// iblDiffuseIntensity/iblSpecularIntensity are independent, not one shared iblIntensity - ported
// from OpenSceneGraph.py/examples/pyosg-lighting/11-sketchfab-lambertian.py's own knobs: turning
// up diffuse SH enough to read as ambient fill also blows out reflections if the two share one
// scalar, and turning reflections down to a sane brightness crushes ambient back to near-black.
// Also the pair a caller dials toward zero to make punctual lights read more clearly against IBL
// - see PBRIBLScene::iblDiffuseIntensity/iblSpecularIntensity and PBRIBLLightingScene's own
// identically-named pair (PBRIBL.hpp) for the live-tunable Uniform refs osgx::gltf::pbribl exposes
// for exactly this, one per lighting-pass shape.
//
// iblAxis is the KTX/OpenGL cubemap lookup basis - a prepared environment may override this for
// a legacy or application-specific cube convention.
inline constexpr const char* IBL_LIGHTING_INPUTS = R"GLSL(
uniform samplerCube envMap;
uniform sampler2D brdfLUT;
uniform samplerCube diffuseEnv;
uniform float iblDiffuseIntensity;
uniform float iblSpecularIntensity;
uniform vec3 iblAxis[3];
)GLSL";

// osgx_EvaluateIBL() - the production diffuse+specular IBL evaluator osgx::gltf::pbribl's own
// forward and deferred lighting shaders both run (formerly a private, unprefixed `evaluateIBL()`/
// `Lighting` duplicated verbatim in PBRIBL.cpp's two shader strings - moved here 2026-08-22 so a
// `Hook::DeferredLighting` override can actually reuse it via `#pragma osgx::ibl IBL_LIGHTING_INPUTS,
// EVALUATE_IBL`, instead of hand-copying it the way examples/osgx-gbuffer-comic.cpp's comment used
// to note as simply a fact of life). Requires osgx_Material (osgx::pbr MATERIAL_STRUCT) and
// osgx_F_MultiScatter (osgx::pbr F_MULTISCATTER) already in scope, plus IBL_LIGHTING_INPUTS above.
//
// Distinct from osgx_AmbientLighting()/osgx_IBLSpecular() (PBR.hpp) - those are the simpler,
// specular-only ambient hook a hand-assembled minimal PBR shader uses (see PBR.hpp's own
// AMBIENT_LIGHTING_DECL comment); this is the fuller evaluator for a caller that already has (or
// is willing to bake) a real diffuse irradiance cubemap and wants diffuse/specular kept separate
// (osgx_Lighting, not one blended vec3) - e.g. for a diagnostics isolation mode, or independent
// diffuse/specular intensity tinting.
//
// `N`/`V` MUST already be WORLD-SPACE - unlike osgx_DirectLighting() (which takes worldPos and
// does its own light-vector math internally), this performs no view->world rotation itself. A
// forward-pass caller needs world-space N/V for osgx_DirectLighting() anyway, so compute the
// rotation once and pass the same vectors to both, rather than rotating twice.
inline constexpr const char* EVALUATE_IBL = R"GLSL(
struct osgx_Lighting {
	vec3 diffuse;
	vec3 specular;
};

// KTX/OpenGL cubemap lookup basis. `iblAxis` may be pre-folded for a caller's own coordinate
// convention (e.g. osgx::gltf::pbribl's Z-up -> glTF/Y-up fold, see foldZUpToGLTFAxis() in
// PBRIBL.hpp), so callers pass a raw world-space vector directly, no separate conversion needed.
vec3 osgx_OrientIBL(vec3 d) {
	return vec3(dot(d, iblAxis[0]), dot(d, iblAxis[1]), dot(d, iblAxis[2]));
}

osgx_Lighting osgx_EvaluateIBL(osgx_Material mat, vec3 N, vec3 V) {
	osgx_Lighting result;

	vec3 diffuseIrradiance = texture(diffuseEnv, osgx_OrientIBL(N)).rgb;

	// The KTX2 prefilter has a terminal level that is not part of the Khronos GGX chain. Match
	// the reference viewer: roughness 1 selects the last filtered level, not that terminal level.
	float maxMip = float(max(textureQueryLevels(envMap) - 2, 0));
	vec3 R = reflect(-V, N);
	vec3 R_gl = osgx_OrientIBL(R);
	vec3 prefiltered = textureLod(envMap, R_gl, mat.roughness * maxMip).rgb;

	// Matches pyosg-khronos-viewer.py's fresnel()/fd/fm/mix(...) exactly: two independent
	// multiscatter-Fresnel evaluations (dielectric F0=0.04, metal F0=albedo), mixed by metallic
	// AFTER Fresnel - NOT a single Fresnel evaluation of a pre-blended F0=mix(0.04,albedo,metallic).
	// The two are not equivalent: F_MultiScatter is nonlinear in F0 (see its Favg/(1-roughness)
	// clamp terms), so mixing F0 first and evaluating Fresnel once diverges from evaluating
	// Fresnel twice and mixing the result - most visible on partially-metallic materials at
	// grazing angles/higher roughness. This was the actual source of a real, camera-independent
	// specular mismatch found comparing BoomBox's handle (non-trivial metallic in its
	// metallicRoughnessTexture) against the Khronos reference.
	vec3 Fd = osgx_F_MultiScatter(N, V, mat.roughness, vec3(0.04), brdfLUT);
	vec3 Fm = osgx_F_MultiScatter(N, V, mat.roughness, mat.albedo, brdfLUT);
	vec3 kD_ibl = (1.0 - Fd) * (1.0 - mat.metallic);

	result.diffuse = diffuseIrradiance * mat.albedo * kD_ibl * mat.ao * iblDiffuseIntensity;
	result.specular = prefiltered * mix(Fd, Fm, mat.metallic) * mat.ao * iblSpecularIntensity;

	return result;
}
)GLSL";

void registerIBLShaderLibs();

}
