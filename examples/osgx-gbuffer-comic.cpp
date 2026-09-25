// vimrun! ./examples/osgx-gbuffer-comic model.gltf
//
// A "comic book" cel-shaded rendering style built entirely on osgx::Hook::DeferredLighting: a
// caller-supplied fragment shader REPLACING PBRLightingPass::create()'s entire lighting-pass
// main(), not one leaf function - the "I know what I'm doing, rewrite the whole pipeline" escape
// hatch described in Shader.hpp's own Hook::DeferredLighting comment and
// PBRLightingPassOptions' own comment (PBRDeferred.hpp).
//
// The custom shader below never calls osgx_DirectLighting()/osgx_EvaluateEnvironment() at all - it reads the
// G-buffer via osgx_GetGBuffer() (#pragma osgx::gbuffer DEFERRED_LIGHTING_INPUTS, GET_GBUFFER --
// the same structured decode PBRLightingPass::create()'s own built-in default uses internally)
// and renders: quantized diffuse bands with a warm-lit/cool-shadow color-graded tint; procedural
// cross-hatching (scale-calibrated from the loaded model's own bounding radius, no texture asset
// needed); a metal-aware specular highlight, anti-aliased via osgx_SpecularAA()
// (gb.roughness/gb.metallic - this shader looks best on non-metallic/low-reflectivity materials,
// since it has no real reflections to fall back on for glossy metal); black ink outlines from
// G-buffer edge detection (comparing each fragment's view-space normal/depth against its
// 4-neighborhood - the standard deferred-renderer silhouette/crease technique, no extra
// geometry pass needed); and a soft-clamped emissive glow. It DOES still gamma-encode its final
// output (a display-referred correctness requirement, not a style choice - osgx_Tonemap() itself
// stays unused, see
// PBRLightingPassOptions' own tonemap/hooks comment for why those are two separate decisions).
//
// No environment/IBL, no LightSet, no ShadowMap - PBRLightingPass::create()'s `environment`
// parameter is OPTIONAL specifically so a Hook::DeferredLighting override like this one, which
// never samples an osgx::Environment, doesn't have to pay for an HDR bake or KTX2 load that
// would go entirely unused (see that function's own comment, PBRDeferred.hpp/.cpp). The light itself is
// a single plain `sunDirection` uniform, live-draggable via ImGui - not a real osgx::LightSet.
//
// Deliberately skips osgx-gbuffer.cpp's shadow camera, floor, and G-buffer channel viewer. DOES
// include an ImGui panel (OSGX_IMGUI builds only) for live tuning of every knob below, same pattern
// osgx-gbuffer.cpp/osgx-shadow.cpp already use. See TODO.md's stylized-rendering entry for where
// this is headed next.
//
// HATCH TECHNIQUE, 2026-08-23: replaced the old sin()-based hatchStripe() (a single perfect
// stripe per direction, gated by the quantized `band`) with hatchFamily() - a port of
// examples/pyosg-hatch.py's fullscreen-quad hatching PoC (OpenSceneGraph.py repo), confirmed
// live there as producing genuinely hand-drawn-reading crosshatch: fbm domain warp (keeps
// strokes from looking ruler-straight), per-row width jitter, along-the-line breaks, and fine
// nib/dropout noise, all normalized by the stripe SPACING itself so the noise frequencies stay
// scale-invariant across differently-sized models - same "measure relative to the
// artist-controlled spacing, not raw world units" principle hatchFrequencyFor() below already
// used for the base frequency alone. Three independently-tunable families (h1/h2/h3, each its
// own angle/spacing-scale/thickness-scale/seed) replace the old fixed +45/-45 pair, each gated
// by its own onset RANGE against `hatchDarkness` (continuous 1-NdotL, deliberately NOT the
// quantized `band` - see that variable's own comment) - this reproduces the classic layered
// crosshatch progression (one direction first, a second crossing it deeper in shadow, a third
// fine family only in the darkest corners) driven by tone, the way a hand-inker actually
// decides where to lay lines.
//
// This also fixes the KNOWN ISSUE the previous version of this file flagged here (confirmed
// live 2026-08-21): `hatch *= mix(1.0, ao, aoMask)` DIMMED hatch in occluded crevices rather
// than ERASING it. AO/SSAO alone is also the wrong PRIMARY signal for gating hatch at all --
// occlusion answers "is this point geometrically enclosed," a fact about cavity shape entirely
// independent of whether light is hitting it; gating on it puts crosshatch on brightly-lit
// crevices and reads as an error, not style. `hatchDarkness` (tone) is now the primary gate;
// AO is a secondary GATE (see aoEraseLow/aoEraseHigh below) that only erases hatch to a clean
// solid fill in the very deepest, most-enclosed pockets - a much narrower, safer claim.
// `metallicSuppress` is the third signal: metal reads through its highlight, not pen strokes
// (this file's own header always said this style "looks best on non-metallic" - now it's an
// explicit multiplicative suppression instead of an unaddressed limitation).
//
// NOTE on "AO Mask": now combines TWO genuinely different occlusion signals, multiplicatively --
// same convention the built-in lighting shader's own `mat.ao *= texture(aoTex, vUV).r;` uses
// (PBRDeferred.cpp). `gb.ao` is the glTF material's BAKED occlusion texture value
// (osgx_GetMaterial()'s mat.ao, PBR.hpp), defaulting to a flat 1.0 (fully unoccluded) when
// the loaded model has no dedicated occlusion texture - CONFIRMED 2026-08-21 as why the slider
// once looked dead on Batman (a non-Khronos custom asset with no occlusion texture at all), not a
// pipeline bug: it works correctly on models that actually carry one, e.g. the Khronos
// `CompareAmbientOcclusion` sample (purpose-built for exactly this A/B) or `DamagedHelmet`. `aoTex`
// (gated behind `OSGX_PBR_AO`, set automatically when `PBRLightingPassOptions::aoTexture` is
// non-null) is real-time screen-space AO from `osgx::SSAO::create()` (GBuffer.hpp) - catches
// self/contact occlusion the static bake can't (this model resting against itself, or against
// nothing else in this shadowless/lightless scratchpad, but the mechanism is general). Combining
// both into one `hatch *= mix(1.0, ao, aoMask);` mask means a crevice reads as fully occluded if
// EITHER signal says so, matching how real ink illustration treats "this area is dark" as one
// binary decision regardless of why it's dark.
// osgx-gbuffer.cpp's channel viewer still doesn't expose gb.ao (gAlbedo's alpha) as its own mode
// - worth adding once/if it's useful for debugging which models have baked occlusion.
//
// NOT YET IMPLEMENTED - "hatch cloud" outside the silhouette (design intent only, 2026-08-23):
// stylized crosshatch in a ~50px screen-space ring OUTSIDE the model's own silhouette, following
// it like a living aura rather than stopping dead at the model's edge. Everything in this file
// today gates hatch by data that only exists WHERE the model was actually rendered (gb.normal,
// hatchDarkness from NdotL, etc.) - background pixels are `discard`ed outright (the "cleared-
// but-never-written pixel" test at the top of main()), so extending hatch past the silhouette
// needs a genuinely new upstream pass, not just a shader tweak to this file's existing main().
//   1. Inside/outside itself is already free - it's the exact same "was this pixel written by
//      the geometry pass" test main() already discards on. No per-model bookkeeping needed for
//      that part.
//   2. The "how far outside, up to some pixel radius" part needs a real 2D screen-space
//      proximity field - raw depth-buffer comparison can't answer "how many screen pixels away
//      am I from the silhouette," it has no notion of 2D distance. Jump Flooding Algorithm (JFA)
//      is the standard real-time technique: a handful of full-screen ping-pong passes
//      (~log2(radius), so ~6 for 50px) where each pass propagates "nearest silhouette pixel"
//      outward at doubling step sizes - O(log radius) cost, not O(radius^2), so a 100px cloud
//      costs one more pass, not 4x the work.
//   3. Propagate more than a scalar distance through the JFA - carry the nearest silhouette
//      pixel's own worldPos (and maybe normal) forward too, so background pixels inside the ring
//      have a coherent world-anchored 2D coordinate to feed hatchCoord/hatchFamily() with,
//      instead of falling back to raw screen-space coordinates - which would reintroduce the
//      exact "decal stuck to the screen, slides as the camera orbits" bug hatchCoord's own
//      comment in main() already documents fixing for on-model hatch.
//   4. Reuse, don't reinvent, the existing gating - once a per-pixel `distanceToSilhouette` (in
//      screen pixels) exists, `smoothstep(50.0, 0.0, distance)` is a drop-in replacement for
//      `hatchDarkness` on ring pixels, so the SAME h1/h2/h3 onset-range machinery this file
//      already has decides how the cloud thins out toward its outer edge.
//   5. Depth-occlusion (would a nearer, unrelated object block part of the ring?) is a non-issue
//      for THIS single-model scratchpad specifically - background pixels here are genuinely
//      empty, nothing else is ever drawn there to be occluded by. Only relevant if this shader is
//      ever reused in a multi-object scene with real geometry behind/around the hatched model.

#include "osgx/Core.hpp"
#include "osgx/GBuffer.hpp"
#include "osgx/gltf/Environment.hpp"
#include "osgx/PBRDeferred.hpp"
#include "osgx/ImGui.hpp"
#include "osgx/Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/ComputeBoundsVisitor>
#include <osg/DisplaySettings>
#include <osg/Program>
#include <osg/Shader>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int WIDTH = 1280;
constexpr int HEIGHT = 800;

std::filesystem::path findModelFile(std::string_view filename) {
	if(auto path = osgx::findDataFile(filename); !path.empty()) return path;

	const std::filesystem::path requested(filename);

	return osgx::findDataFile(
		requested.stem().string(), {"glTF-Sample-Assets/Models/{}/glTF/{}.gltf"}
	);
}

// Refreshes the lighting pass's view-matrix uniforms every frame - same requirement as
// osgx-gbuffer.cpp's own UpdateLightingPassCallback, just as a lambda-driven NodeCallback here
// since there's no shadow camera in this scratchpad to hang it off of as a preDrawCallback.
// `ssaoProjection` mirrors osgx-gbuffer.cpp's own addition - see osgx::SSAO::create()'s doc
// comment (GBuffer.hpp) for why SSAO needs its own freshly-updated forward-projection uniform.
class UpdateLightingPassCallback: public osg::Camera::DrawCallback {
public:
	UpdateLightingPassCallback(
		osgx::PBRLightingPass* scene,
		osg::Camera* mainCamera,
		osg::Uniform* ssaoProjection=nullptr
	):
		_scene(scene), _mainCamera(mainCamera), _ssaoProjection(ssaoProjection) {}

	void operator()(osg::RenderInfo&) const override {
		_scene->update(_mainCamera.get());

		if(_ssaoProjection.valid()) {
			_ssaoProjection->set(osg::Matrixf(_mainCamera->getProjectionMatrix()));
		}
	}

private:
	osgx::PBRLightingPass* _scene;
	osg::observer_ptr<osg::Camera> _mainCamera;
	osg::observer_ptr<osg::Uniform> _ssaoProjection;
};

// The osgx::Hook::DeferredLighting override itself. Requires nothing but the G-buffer contract --
// no LightSet, no ShadowMap, no IBL environment - see the file-level comment above.
constexpr const char CUSTOM_DEFERRED_LIGHTING_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core
#pragma osgx::gbuffer DEFERRED_LIGHTING_INPUTS, GET_GBUFFER
// osgx_SpecularAA() - see its call site in main() below for why the specular highlight needs it.
#pragma osgx::pbr SPECULAR_AA

// World-space "sun" direction, live-draggable via ImGui - NOT an osgx::LightSet (this
// scratchpad still has none), just a single plain uniform main() reads directly. Default matches
// the original hardcoded value: normalize(vec3(1, 1, 2)).
uniform vec3 sunDirection;
// Global on/off - skips the entire hatch computation (all three hatchFamily() calls, not just
// zeroing the result afterward) so it's also a cheap A/B toggle, not merely a visual one.
uniform bool hatchEnabled;
const int BANDS = 4;
const float TWO_PI = 6.28318530718;
// Classic comic/illustration color grading - shadow areas tint slightly cool (as if lit by
// ambient sky/bounce light), lit areas tint slightly warm (as if lit by a warm key light). A
// small, cheap multiplicative color shift on top of the existing brightness bands; NOT a real
// ambient light source (no LightSet, see the file header), just a stylized tint.
const vec3 SHADOW_TINT = vec3(0.55, 0.65, 0.85);
const vec3 LIGHT_TINT = vec3(1.05, 1.0, 0.92);
// Soft-clamp exposure for emissive - without osgx_Tonemap() at all in this shader, a raw HDR
// emissive value fed straight to the framebuffer can blow out past 1.0 and clip harshly. A
// simple Reinhard-style 1-exp(-x) curve keeps bright glow readable without a hard clip, without
// pulling in the full osgx_Tonemap() machinery this shader deliberately doesn't use.
const float EMISSIVE_EXPOSURE = 1.5;
const float EDGE_NORMAL_THRESHOLD = 0.4;
const float EDGE_DEPTH_THRESHOLD = 0.05;
// Outline width, in texels - widening the neighbor-sample radius means any fragment within
// this many texels of a real discontinuity has a neighbor sample that crosses it, thickening
// the resulting ink line proportionally (not a separate blur/dilate pass).
const float EDGE_THICKNESS = 4.0;
// Hatch stripe spacing, in WORLD units (not screen pixels - see hatchCoord's own comment in
// main() for why). WORLD units means this can't be one fixed constant across models of wildly
// different physical scale (confirmed live: DamagedHelmet vs. Fox vs. a unit Cube all needed
// completely different values at the same raw number) - so this is now a uniform, set once at
// startup in main() from the loaded model's own bounding radius (--hatch-density scales it).
// hatchFamily() below reads it as 1/hatchFrequency - a stripe PERIOD, in world units - and
// normalizes every family's local coordinate by its own (period * per-family scale) so the warp/
// break/nib noise frequencies stay scale-invariant regardless of hatchFrequency's actual value.
uniform float hatchFrequency;
// Base line thickness, as a fraction of one stripe period (unchanged meaning from the original
// sin()-based version) - each family scales this by its own h{1,2,3}ThicknessScale.
uniform float hatchThickness;
// Darkening strength (1.0 = no darkening, i.e. an easy way to A/B hatching on/off without
// recompiling) - also a uniform, also CLI-fed (--hatch-thickness/--hatch-darken), same
// reasoning as hatchFrequency above.
uniform float hatchDarken;
// Erases (masks) hatching in heavily-occluded crevices, 0..1 - 0 leaves hatching exactly as
// hatchDarkness/per-family onset alone produces it (today's default); 1 fully gates hatch by
// aoEraseLow/aoEraseHigh below, so a deeply occluded crevice shows solid dark fill instead of
// crosshatch lines. This is a SECONDARY signal, not the primary gate - see the file header for
// why AO alone is the wrong thing to drive hatch placement from.
uniform float aoMask;
// AO GATE thresholds (ao below aoEraseLow -> hatch fully erased; above aoEraseHigh -> untouched;
// smooth in between) - replaces the old `mix(1.0, ao, aoMask)` DIM with a real ERASE (see file
// header's fixed KNOWN ISSUE).
uniform float aoEraseLow;
uniform float aoEraseHigh;
// Metal reads through its highlight, not pen strokes - this multiplicatively suppresses hatch
// coverage by gb.metallic (0 = no suppression, 1 = fully suppress on pure metal). A blend, not a
// hard cutoff, so a texel that's only partially metallic (common at material edges in glTF
// assets) doesn't get a binary hatch/no-hatch seam.
uniform float metallicSuppress;

// Shared "stroke character" knobs - every hatchFamily() call reads the SAME values here, same
// as pyosg-hatch.py's global Stroke Character section (these constants shaped that PoC's
// hand-drawn feel directly; porting the knobs, not just the math, is the point).
uniform float warpAmount;
uniform float rowJitter;
uniform float widthJitter;
uniform float breakLow;
uniform float breakHigh;
uniform float nibLow;
uniform float nibHigh;
// Minimum antialiasing band width, in units of one stripe period - fwidth(localY) alone can
// collapse toward 0 at a glancing view angle or on a very fine hatch, aliasing the line edge;
// this floors it. In NORMALIZED (period-relative) units, unlike pyosg-hatch.py's pixel-space
// `max(fwidth(localY), 0.75)` - that literal 0.75 doesn't carry over (its own spacing was
// ~10px, so 0.75/10 =~ 0.075 of a period is the equivalent floor; 0.08 below matches that).
uniform float hatchAA;

// Per-family knobs - angle (degrees), spacing/thickness SCALE (multiplies hatchFrequency's
// period / hatchThickness respectively), a noise seed (keeps each family's warp/break/nib
// pattern independent), and the darkness RANGE (against hatchDarkness) where this family turns
// on. Defaults: h1 turns on first (mid shadow), h2 crosses it deeper in, h3 (denser, thinner)
// only in the darkest corners - the classic layered crosshatch progression.
uniform float h1Angle, h1SpacingScale, h1ThicknessScale, h1Seed, h1OnsetLow, h1OnsetHigh;
uniform float h2Angle, h2SpacingScale, h2ThicknessScale, h2Seed, h2OnsetLow, h2OnsetHigh;
uniform float h3Angle, h3SpacingScale, h3ThicknessScale, h3Seed, h3OnsetLow, h3OnsetHigh;

// Real-time SSAO - see the file header's "NOTE on AO Mask" for how this combines with gb.ao.
// Gated the same way the built-in lighting shader gates it (OSGX_PBR_AO, set automatically by
// PBRLightingPassOptions::aoTexture when non-null) - so this shader still compiles/runs
// unchanged if no SSAO pass is ever wired in. `#pragma import_defines` IS required here (confirmed
// against OSG's own osg::Shader::_shaderDefines/getDefineString(): a StateSet define is only
// written into a given Shader's compiled source if that Shader itself declares the define name via
// import_defines - otherwise `#ifdef OSGX_PBR_AO` is always false regardless of what the
// StateSet has set). Same reasoning LIGHTING_FRAGMENT_SHADER_SRC's own
// `#pragma import_defines ( OSGX_PBR_DIAGNOSTICS, OSGX_PBR_NO_TONEMAP, OSGX_PBR_AO )` line
// exists (PBRDeferred.cpp) - every shader that reads a StateSet-driven define needs its own copy of
// this pragma, not just one shader in the Program.
#pragma import_defines ( OSGX_PBR_AO )
#ifdef OSGX_PBR_AO
uniform sampler2D aoTex;
#endif

float hash21(vec2 p) {
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

float valueNoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);

	float a = hash21(i);
	float b = hash21(i + vec2(1.0, 0.0));
	float c = hash21(i + vec2(0.0, 1.0));
	float d = hash21(i + vec2(1.0, 1.0));

	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p) {
	float v = 0.0;
	float a = 0.5;

	for(int i = 0; i < 4; ++i) {
		v += a * valueNoise(p);
		p = p * 2.03 + vec2(17.1, 9.2);
		a *= 0.5;
	}

	return v;
}

mat2 rot(float a) {
	float c = cos(a);
	float s = sin(a);

	return mat2(c, -s, s, c);
}

// One hand-drawn-reading crosshatch layer over whatever 2D coordinate space `p` is given
// (world-space in main() below, see hatchCoord's own comment for why) - ported from
// pyosg-hatch.py's hatchFamily(), confirmed live there as reading as genuinely hand-inked rather
// than mathematically perfect parallel stripes. `angleDeg` rotates the stripe direction;
// `spacingWorld` is this family's stripe PERIOD in world units (hatchFrequency's period times
// its own h{N}SpacingScale); `thicknessFrac` is line thickness as a fraction of one period
// (hatchThickness times h{N}ThicknessScale); `seed` keeps this family's warp/break/nib pattern
// independent of the others. Returns 1.0 where an ink line falls, 0.0 elsewhere, with smooth
// antialiasing in between.
//
// Works entirely in `qn` - p, ROTATED then divided by spacingWorld, so one unit of qn.y is
// exactly one stripe period. This is the same "measure relative to the artist-controlled
// spacing, not raw world units" principle hatchFrequencyFor() already uses for the base
// frequency - without it, every noise-frequency constant below would need re-tuning per model
// scale, exactly the bug that function exists to avoid for spacing itself. The noise-frequency
// constants themselves are pyosg-hatch.py's own pixel-space values, re-derived into
// period-relative units (multiplied by ~10, that PoC's own typical spacing in pixels).
float hatchFamily(vec2 p, float angleDeg, float spacingWorld, float thicknessFrac, float seed) {
	vec2 q = rot(radians(angleDeg)) * p;
	vec2 qn = q / max(spacingWorld, 1e-5);

	// Large-scale distortion: keeps strokes from looking ruler-straight.
	float warp = warpAmount * (
		(fbm(qn * 0.35 + seed) - 0.5) * 0.5 +
		(fbm(qn * 1.1 + seed * 2.7) - 0.5) * 0.12
	);

	qn.y += warp;

	float row = floor(qn.y);
	float localY = fract(qn.y) - 0.5;

	// Per-stroke width variation.
	float rowRnd = hash21(vec2(row, seed));
	float halfWidth = thicknessFrac * 0.5 * mix(1.0 - widthJitter, 1.0 + widthJitter, rowRnd);

	localY += (hash21(vec2(row, seed + 13.0)) - 0.5) * rowJitter;

	float aa = max(fwidth(localY), hatchAA);
	float line = 1.0 - smoothstep(halfWidth, halfWidth + aa, abs(localY));

	// Break strokes along their length - keeps the result from reading as mathematically
	// perfect parallel stripes.
	float along =
		0.68 * fbm(vec2(qn.x * 0.55, row * 0.37 + seed * 7.0)) +
		0.32 * fbm(vec2(qn.x * 1.7 + 31.0, row * 1.91));

	float broken = smoothstep(breakLow, breakHigh, along);

	// Fine nib/dropout variation.
	float nib = valueNoise(vec2(qn.x * 4.2, qn.y * 3.1) + seed * 19.0);
	nib = smoothstep(nibLow, nibHigh, nib);

	return line * broken * nib;
}

// Ink-outline test: compares this fragment's own view-space normal/depth (N0/depth0) against its
// 4-neighborhood in the G-buffer - flags a silhouette (neighbor is background) or a sharp crease
// (neighbor's normal/depth diverges past threshold). Pure screen-space post-process, no extra
// geometry pass - the same shape the moodboard's own "EDGE DETECTION" pipeline-inspector panel
// showed. `depth0` is view-space Z (negative, camera looking down -Z), so the depth threshold
// scales with distance from camera rather than being one fixed world-space epsilon.
float detectEdge(vec2 uv, vec3 N0, float depth0) {
	vec2 texel = EDGE_THICKNESS / vec2(textureSize(gNormal, 0));
	vec2 offsets[4] = vec2[](
		vec2(texel.x, 0.0), vec2(-texel.x, 0.0), vec2(0.0, texel.y), vec2(0.0, -texel.y)
	);
	float edge = 0.0;

	for(int i = 0; i < 4; i++) {
		vec3 N = texture(gNormal, uv + offsets[i]).rgb;

		if(dot(N, N) < 0.0001) { edge = 1.0; continue; } // neighbor is background -> silhouette

		float depth = texture(gPosition, uv + offsets[i]).z;

		edge = max(edge, step(EDGE_NORMAL_THRESHOLD, 1.0 - dot(N0, normalize(N))));
		edge = max(edge, step(EDGE_DEPTH_THRESHOLD * abs(depth0), abs(depth0 - depth)));
	}

	return edge;
}

void main() {
	osgx_GBuffer gb = osgx_GetGBuffer(vUV);

	// Same "cleared-but-never-written pixel" background test the built-in default uses.
	if(dot(gb.normal, gb.normal) < 0.0001) discard;

	vec3 N_view_n = normalize(gb.normal);
	mat3 invView = transpose(mat3(osgx_mainViewMatrix));
	vec3 N = invView * N_view_n;
	vec3 V = normalize(invView * normalize(-gb.position));

	// Quantized "toon" diffuse band - no osgx_DirectLighting(), no osgx_EvaluateEnvironment(), no
	// osgx_Tonemap() anywhere in this file. Proves the override really does replace the whole
	// pass rather than layering onto the built-in.
	float NdotL = max(dot(N, sunDirection), 0.0);
	float band = ceil(NdotL * float(BANDS)) / float(BANDS);
	vec3 shaded = gb.albedo * mix(0.25, 1.0, band) * mix(SHADOW_TINT, LIGHT_TINT, band);

	// Cross-hatch, applied to the base diffuse term ONLY - deliberately computed and multiplied
	// in BEFORE the rim/specular accents added below, so those stay clean, unhatched "highlight"
	// accents instead of getting muddied by ink lines.
	//
	// hatchDarkness is CONTINUOUS (1-NdotL), deliberately NOT `band` - `band` is already
	// quantized for the flat color fill above, and gating hatch onset from an already-quantized
	// value double-quantizes it, producing harsh onset cliffs instead of the smooth
	// per-family onset RANGE below actually controls.
	//
	// hatchCoord is WORLD-space, not screen-space (gl_FragCoord) - screen-space was the first
	// attempt, and it looked wrong: the pattern stayed locked to the viewport instead of the
	// surface, visibly sliding across the model as the camera orbited (a decal stuck to the
	// monitor, not painted on the object). World position is genuinely tied to the physical
	// surface point, so the pattern now stays put as the camera moves. Which 2D plane to project
	// onto (XY/XZ/YZ) is picked per-fragment by whichever world axis the surface normal points
	// along LEAST - a cheap "axis-dominant" trick (not full smooth-blended triplanar) that avoids
	// the worst stretching a single fixed plane would cause on faces nearly perpendicular to it
	// (e.g. the helmet's curved dome, mostly Z-facing, would smear badly under a pure XZ or YZ
	// projection).
	vec3 worldPos = (osgx_mainViewMatrixInverse * vec4(gb.position, 1.0)).xyz;
	vec3 absN = abs(N);
	vec2 hatchCoord = (absN.z >= absN.x && absN.z >= absN.y)
		? worldPos.xy
		: (absN.x >= absN.y ? worldPos.yz : worldPos.xz)
	;
	float hatchDarkness = clamp(1.0 - NdotL, 0.0, 1.0);
	// hatchFrequency is tuned as a sin()-style angular frequency (see its own comment and
	// hatchFrequencyFor() in main() - that back-solved K constant is preserved as-is), whose
	// period is 2*PI/frequency, NOT 1/frequency - hatchFamily() uses this as a literal world-
	// space PERIOD, so the 2*PI has to be carried through here to reproduce the same confirmed-
	// good density hatchFrequencyFor() already tunes for.
	float baseSpacing = TWO_PI / max(hatchFrequency, 1e-4);
	float hatch = 0.0;

	// hatchEnabled skips the three hatchFamily() calls entirely, not just their result - a real
	// A/B toggle, not a "compute it then throw it away" one.
	if(hatchEnabled) {
		float h1 = hatchFamily(
			hatchCoord, h1Angle, baseSpacing * h1SpacingScale, hatchThickness * h1ThicknessScale, h1Seed
		);
		float h2 = hatchFamily(
			hatchCoord, h2Angle, baseSpacing * h2SpacingScale, hatchThickness * h2ThicknessScale, h2Seed
		);
		float h3 = hatchFamily(
			hatchCoord, h3Angle, baseSpacing * h3SpacingScale, hatchThickness * h3ThicknessScale, h3Seed
		);

		hatch = max(hatch, h1 * smoothstep(h1OnsetLow, h1OnsetHigh, hatchDarkness));
		hatch = max(hatch, h2 * smoothstep(h2OnsetLow, h2OnsetHigh, hatchDarkness));
		hatch = max(hatch, h3 * smoothstep(h3OnsetLow, h3OnsetHigh, hatchDarkness));

		// Metal reads through its highlight, not pen strokes - see file header and
		// metallicSuppress's own comment. Blend, not a hard cutoff.
		hatch *= mix(1.0, 1.0 - gb.metallic, metallicSuppress);
	}

	// Combined occlusion: baked (gb.ao) * real-time SSAO (aoTex, when wired in) - same
	// multiplicative convention the built-in lighting shader uses for the identical combination.
	float ao = gb.ao;

#ifdef OSGX_PBR_AO
	ao *= texture(aoTex, vUV).r;
#endif

	// AO now GATES (erases) rather than dims - see aoEraseLow/aoEraseHigh's own comment and the
	// file header's fixed KNOWN ISSUE. aoMask still scales how strongly this applies overall,
	// same external meaning the slider always had (0 = no AO effect, 1 = full gate).
	float aoGate = mix(1.0, smoothstep(aoEraseLow, aoEraseHigh, ao), aoMask);

	hatch *= aoGate;
	shaded *= mix(1.0, hatchDarken, hatch);

	float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);

	shaded += gb.albedo * rim * 0.4;

	// Metal-aware "comic shine" - gb.roughness/gb.metallic. Roughness controls the highlight's
	// TIGHTNESS (a Blinn-Phong exponent, not a physically exact GGX-to-Phong conversion - this is
	// a stylized approximation, not a PBR one); metallic controls its COLOR - bare metal tints
	// its highlight by the surface's own albedo (metals have no separate "white" specular layer),
	// while a dielectric surface gets a neutral white one. Two smoothstep-thresholded tiers (a
	// small bright core plus a wider, dimmer rim) instead of a smooth falloff - a hard-edged
	// "graphic" highlight shape matches the flat quantized bands far better than a photorealistic
	// soft glow would.
	//
	// osgx_SpecularAA(N_view_n, gb.roughness) - NOT optional, confirmed live: at a low roughness
	// (high specPower), pow(NdotH, specPower) is extremely sensitive to the tiny per-pixel normal
	// variation a normal map introduces, and the narrow smoothstep thresholds below then amplify
	// that into scattered "boxy" aliased blotches instead of one smooth highlight, worst on
	// heavily normal-mapped metallic surfaces. The built-in PBR path (PBR.hpp/PBRScene.cpp) already
	// calls this for exactly this reason; this shader just wasn't calling it until now.
	float aaRoughness = osgx_SpecularAA(N_view_n, gb.roughness);
	float specPower = mix(128.0, 4.0, aaRoughness);
	vec3 H = normalize(sunDirection + V);
	float spec = pow(max(dot(N, H), 0.0), specPower);
	float specCore = smoothstep(0.35, 0.6, spec);
	float specRim = smoothstep(0.08, 0.3, spec) * 0.4;
	vec3 specColor = mix(vec3(1.0), gb.albedo, gb.metallic);

	shaded += specColor * max(specCore, specRim);

	float edge = detectEdge(vUV, N_view_n, gb.position.z);

	shaded = mix(shaded, vec3(0.02), edge);

	// Soft-clamped emissive (see EMISSIVE_EXPOSURE's own comment), combined BEFORE the gamma
	// encode below - both the shaded surface and the glow need the same linear-to-display
	// conversion, same as the built-in default's own color = surface + direct + emissive;
	// color = osgx_Tonemap(color); ordering (PBRDeferred.cpp), just without the tonemap curve itself.
	vec3 color = shaded + (vec3(1.0) - exp(-gb.emissive * EMISSIVE_EXPOSURE));

	// This shader deliberately never calls osgx_Tonemap() (no tone CURVE - see the file header),
	// but still needs the gamma ENCODE step every display-referred fragment needs before writing
	// to the backbuffer - omitting it (as this shader did until now) makes midtones read
	// noticeably darker/muddier than intended. Two separate decisions, same as
	// PBRLightingPassOptions::tonemap's own comment explains for the built-in path.
	color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));

	fragColor = vec4(color, gb.alphaCoverage);
}
)GLSL";

}

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);
	auto lib = osgx::initialize(args);

	args.getApplicationUsage()->setCommandLineUsage(
		std::string(args.getApplicationName()) + " <model.gltf> [--samples <count>] [hatch options]"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--samples <count>", "Request this many default-framebuffer MSAA samples (default: 4)"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--hatch-density <multiplier>",
		"Scales the hatch line frequency (default: 6.0). The auto-derived default (from the "
		"loaded model's own bounding radius) should look reasonable on any model size without "
		"this; use it to go denser/sparser to taste."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--hatch-thickness <fraction>",
		"Line thickness as a fraction of one stripe period, 0..1 (default: 0.18)."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--hatch-darken <factor>",
		"How much a hatch line darkens the surface under it, 0..1 (default: 0.55). 1.0 disables "
		"the visible effect entirely - a quick way to A/B hatching on/off without recompiling."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--ao-mask <strength>",
		"Erases hatching in occluded crevices by ambient occlusion (baked material AO combined "
		"with real-time SSAO), 0..1 (default: 0.5). 0 leaves hatching exactly as shading-band "
		"density alone produces it; 1 fully masks by AO."
	);
	int samples = 4;
	bool hatchEnabled = true;
	float hatchDensity = 6.0f;
	float hatchThickness = 0.18f;
	float hatchDarken = 0.55f;
	float aoMask = 0.5f;

	args.read("--samples", samples);
	args.read("--hatch-density", hatchDensity);
	args.read("--hatch-thickness", hatchThickness);
	args.read("--hatch-darken", hatchDarken);
	args.read("--ao-mask", aoMask);

	if(args.argc() < 2 || samples < 0) {
		args.getApplicationUsage()->write(std::cerr);

		return 1;
	}

	osg::DisplaySettings::instance()->setNumMultiSamples(static_cast<unsigned int>(samples));
	osgViewer::Viewer viewer(args);

	// Dear ImGui's single global context isn't safe to touch from more than one OSG draw thread --
	// see osgx::imgui::Widget's own class comment; harmless to set unconditionally even when
	// OSGX_IMGUI is off.
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
	viewer.setUpViewInWindow(50, 50, WIDTH, HEIGHT);

	osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");

	const auto modelPath = findModelFile(args[1]);
	osg::ref_ptr<osg::Node> model;

	if(!modelPath.empty()) model = osgDB::readRefNodeFile(modelPath.string());

	if(!model) {
		std::cerr << "Failed to load: " << args[1] << std::endl;

		return 1;
	}

	// Target hatch stripes-per-bounding-radius, regardless of the loaded model's real-world
	// scale - confirmed live that a single fixed world-space frequency looks wildly different
	// (huge on DamagedHelmet, invisible on Fox) across differently-scaled assets.
	// --hatch-density scales this multiplicatively for further to-taste tuning.
	//
	// The 197 below is back-solved, not guessed: the very first world-space attempt used a FIXED
	// HATCH_FREQUENCY=120 and looked genuinely good on DamagedHelmet before per-model scale
	// differences were noticed. That model's own boundRadius, measured live, is 1.6446 - so the
	// target this formula needs to reproduce that exact good-looking result is K = 120 * 1.6446 =
	// 197.35. K stays constant relative to boundRadius by construction, so this reproduces the
	// SAME visual density on every model, Fox's 53x larger boundRadius included.
	osg::ComputeBoundsVisitor boundsVisitor;

	model->accept(boundsVisitor);

	const auto& bounds = boundsVisitor.getBoundingBox();
	const float boundRadius = bounds.valid() ? bounds.radius() : 1.0f;
	// Shared by the initial CLI-driven value below AND the ImGui density slider further down --
	// one formula, not two copies that could drift.
	auto hatchFrequencyFor = [boundRadius](float density) {
		return density * 197.35f / (boundRadius > 0.001f ? boundRadius : 0.001f);
	};
	const float hatchFrequency = hatchFrequencyFor(hatchDensity);
	// Live-draggable via ImGui below - default matches the shader's own original hardcoded
	// value, normalize(vec3(1, 1, 2)).
	osg::Vec3 sunDirection(0.4082483f, 0.4082483f, 0.8164966f);

	auto gbuffer = osgx::PBRGBuffer::create(model, WIDTH, HEIGHT);

	if(!gbuffer.valid()) {
		std::cerr << "Failed to build the G-buffer geometry pass" << std::endl;

		return 1;
	}

	// SSAO: built before lightingOptions/PBRLightingPass::create() specifically so its
	// aoTexture can be set on lightingOptions normally, rather than the "wire it in by hand
	// afterward" workaround an SSAO-after-the-lighting-pass ordering would need (see
	// PBRLightingPassOptions::aoTexture's own comment, PBRDeferred.hpp, for how that seam expects to
	// be used). Reads gbuffer's normal/position directly - both already exist once the geometry
	// pass above is built. Radius scaled off the model's own bound, same "derive from the model's
	// own bounds" precedent hatchFrequencyFor() above already uses - a fixed radius tuned for one
	// model looks wrong at a very different scale.
	auto ssaoProjection = osgx::make_ref<osg::Uniform>(
		"projectionMatrix", osg::Matrixf::identity()
	);
	const float ssaoRadius = std::max(0.05f, boundRadius * 0.15f);

	auto ssao = osgx::SSAO::create(
		gbuffer.normalTexture, gbuffer.positionTexture, ssaoProjection.get(), WIDTH, HEIGHT, ssaoRadius
	);

	if(!ssao.valid()) {
		std::cerr << "Failed to build the SSAO pass" << std::endl;

		return 1;
	}

	osgx::PBRLightingPassOptions lightingOptions;

	// The seam CUSTOM_DEFERRED_LIGHTING_FRAGMENT_SHADER's own `#ifdef OSGX_PBR_AO` block reads
	// - setting this here (same as any other PBRLightingPassOptions consumer) is what makes
	// PBRLightingPass::create() bind aoTex/unit 8/OSGX_PBR_AO on the StateSet at all; the
	// custom shader still has to declare and sample `aoTex` itself, since Hook::DeferredLighting
	// REPLACES main() rather than inheriting the built-in's own declarations.
	lightingOptions.aoTexture = ssao.aoTexture.get();

	lightingOptions.hooks = {{
		osgx::Hook::DeferredLighting,
		new osg::Shader(
			osg::Shader::FRAGMENT,
			// resolveShaderLibs() is what expands `#pragma osgx::gbuffer DEFERRED_LIGHTING_INPUTS,
			// GET_GBUFFER` above into real GLSL - passing raw, un-expanded source straight to
			// osg::Shader() leaves the literal `#pragma` line in place, which the driver then
			// chokes on (and every symbol the pragma was supposed to declare - gb,
			// osgx_mainViewMatrix, fragColor - comes back "undefined").
			osgx::resolveShaderLibs(CUSTOM_DEFERRED_LIGHTING_FRAGMENT_SHADER)
		)
	}};

	// No environment: this style never samples IBL (see the file-level comment).
	auto lighting = osgx::PBRLightingPass::create(gbuffer, viewer.getCamera(), lightingOptions);

	if(!lighting.valid()) {
		std::cerr << "Failed to build the lighting pass" << std::endl;

		return 1;
	}

	// hatchFrequency/hatchThickness/hatchDarken/aoMask/sunDirection all live on the lighting pass's
	// own StateSet - that's where CUSTOM_DEFERRED_LIGHTING_FRAGMENT_SHADER's `uniform`
	// declarations expect to find them, same as any other uniform PBRLightingPass::create()
	// itself wires up (osgx_mainViewMatrix, etc.). Kept as named pointers (not fire-and-forget) so
	// the ImGui section below can push live edits into the SAME osg::Uniform objects.
	auto* lightingSS = dynamic_cast<osg::Camera*>(lighting.node.get())->getOrCreateStateSet();
	auto* hatchEnabledUniform = new osg::Uniform("hatchEnabled", hatchEnabled);
	auto* hatchFrequencyUniform = new osg::Uniform("hatchFrequency", hatchFrequency);
	auto* hatchThicknessUniform = new osg::Uniform("hatchThickness", hatchThickness);
	auto* hatchDarkenUniform = new osg::Uniform("hatchDarken", hatchDarken);
	auto* aoMaskUniform = new osg::Uniform("aoMask", aoMask);
	auto* sunDirectionUniform = new osg::Uniform("sunDirection", sunDirection);

	lightingSS->addUniform(hatchEnabledUniform);
	lightingSS->addUniform(hatchFrequencyUniform);
	lightingSS->addUniform(hatchThicknessUniform);
	lightingSS->addUniform(hatchDarkenUniform);
	lightingSS->addUniform(aoMaskUniform);
	lightingSS->addUniform(sunDirectionUniform);

	// Every new hatch uniform below (stroke character, per-family, gate) is created and given a
	// default HERE, unconditionally - the shader needs real values regardless of whether an
	// ImGui panel exists to tune them live (an unset GLSL uniform defaults to 0, which breaks
	// this shader outright: zero-width onset ranges collapse every smoothstep to a hard step,
	// zero thickness erases every line). The label/slider-range metadata used to build the ImGui
	// panel is assembled separately, further down inside #ifdef OSGX_IMGUI, by looking these same
	// uniforms back up by name - keeping the single (name, default) list below as the one source
	// of truth avoids maintaining the numbers in two places for a build with ImGui compiled out.
	// Mirrors examples/pyosg-hatch.py's own PARAMS/DEFAULTS split (OpenSceneGraph.py repo).
	const std::vector<std::pair<std::string, float>> hatchDefaults = {
		{"warpAmount", 1.0f}, {"rowJitter", 0.28f}, {"widthJitter", 0.45f},
		{"breakLow", 0.36f}, {"breakHigh", 0.58f}, {"nibLow", 0.18f}, {"nibHigh", 0.42f},
		{"hatchAA", 0.08f},
		{"metallicSuppress", 1.0f}, {"aoEraseLow", 0.15f}, {"aoEraseHigh", 0.5f},
		// h3 (deepest shadow) defaults denser/thinner than h1/h2, the classic "tightest
		// crosshatch in the darkest corners" look. Onset ranges are staggered so h1 turns on
		// first (mid shadow), h2 crosses it deeper in, h3 only joins in the darkest corners.
		{"h1Angle", 45.0f}, {"h1SpacingScale", 1.0f}, {"h1ThicknessScale", 1.0f},
		{"h1Seed", 1.0f}, {"h1OnsetLow", 0.15f}, {"h1OnsetHigh", 0.4f},
		{"h2Angle", -45.0f}, {"h2SpacingScale", 1.0f}, {"h2ThicknessScale", 1.0f},
		{"h2Seed", 9.0f}, {"h2OnsetLow", 0.4f}, {"h2OnsetHigh", 0.65f},
		{"h3Angle", 80.0f}, {"h3SpacingScale", 0.7f}, {"h3ThicknessScale", 0.85f},
		{"h3Seed", 23.0f}, {"h3OnsetLow", 0.65f}, {"h3OnsetHigh", 0.9f},
	};

	for(const auto& [name, def] : hatchDefaults) lightingSS->addUniform(new osg::Uniform(name.c_str(), def));

	auto root = osgx::make_ref<osg::Group>();

	// gbuffer.gbuffer.camera is the FIRST PRE_RENDER camera in this scene graph (added before
	// lighting.node below) - see UpdateLightingPassCallback's own comment (and
	// PBRLightingPass::create()'s) for why the update() call has to land here, not on
	// lighting.node's own preDrawCallback or as a post-frame() application call.
	gbuffer.gbuffer.camera->setPreDrawCallback(
		new UpdateLightingPassCallback(&lighting, viewer.getCamera(), ssaoProjection.get())
	);

	// Add order matters: gbuffer.gbuffer.camera, ssao.rawCamera, and ssao.blurCamera are all
	// PRE_RENDER at the same default order number (0), so OSG breaks the tie by scene-graph add
	// order - ssao.rawCamera/blurCamera MUST come after gbuffer.gbuffer.camera (they read its
	// normal/position output) and before lighting.node (which reads ssao.aoTexture back via
	// aoTex, wired above through lightingOptions.aoTexture).
	root->addChild(gbuffer.gbuffer.camera);
	root->addChild(ssao.rawCamera);
	root->addChild(ssao.blurCamera);
	root->addChild(lighting.node);

	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.addEventHandler(new osgViewer::StatsHandler());
	// viewer.getCamera()->setClearColor(osg::Vec4f(25.0f / 255.0f, 50.0f / 255.0f, 75.0f / 255.0f, 1.0f));
	viewer.getCamera()->setClearColor(osg::Vec4f(180.0f / 255.0f, 180.0f / 255.0f, 180.0f / 255.0f, 1.0f));

	std::cout
		<< "osgx-gbuffer-comic: osgx::Hook::DeferredLighting comic-style shading - quantized bands, "
		<< "noisy hatching, ink outlines, no lights/shadow/IBL evaluation" << std::endl
		<< " AO Mask now combines baked material AO with real-time osgx::SSAO" << std::endl
	;

#ifdef OSGX_IMGUI
	// drawCamera pinned to lighting.node's own Camera explicitly - lighting.node is a nested
	// POST_RENDER camera (a scene-graph child of root, not a viewer slave), and it draws AFTER
	// the master camera's own PostDrawCallback fires. Left at the default (drawCamera=nullptr),
	// the panel would draw via the master camera and then get painted over every frame - same
	// gotcha osgx-gbuffer.cpp's own Widget setup documents at its own call site.
	auto* gui = new osgx::imgui::Widget(viewer, dynamic_cast<osg::Camera*>(lighting.node.get()));

	gui->addSection("Comic Style", [
		hatchFrequencyFor, &hatchEnabled, &hatchDensity, &hatchThickness, &hatchDarken, &aoMask,
		&sunDirection, hatchEnabledUniform, hatchFrequencyUniform, hatchThicknessUniform,
		hatchDarkenUniform, aoMaskUniform, sunDirectionUniform
	](osg::RenderInfo&) {
		if(ImGui::Checkbox("Hatch Enabled", &hatchEnabled)) {
			hatchEnabledUniform->set(hatchEnabled);
		}

		ImGui::Separator();

		if(ImGui::SliderFloat("Hatch Density", &hatchDensity, 0.1f, 10.0f)) {
			hatchFrequencyUniform->set(hatchFrequencyFor(hatchDensity));
		}

		if(ImGui::SliderFloat("Hatch Thickness", &hatchThickness, 0.0f, 0.5f)) {
			hatchThicknessUniform->set(hatchThickness);
		}

		if(ImGui::SliderFloat("Hatch Darken", &hatchDarken, 0.0f, 1.0f)) {
			hatchDarkenUniform->set(hatchDarken);
		}

		if(ImGui::SliderFloat("AO Mask", &aoMask, 0.0f, 1.0f)) {
			aoMaskUniform->set(aoMask);
		}

		// A dragged slider can pass through (0,0,0), which normalizes to NaN - same guard
		// osgx-gbuffer.cpp/osgx-shadow.cpp's own light-drag sections use: fall back to a fixed
		// safe direction for the UNIFORM specifically, without resetting the slider's own
		// displayed (possibly still-degenerate) value.
		if(ImGui::SliderFloat3("Light Direction", sunDirection.ptr(), -1.0f, 1.0f)) {
			if(sunDirection.length2() > 1e-8f) {
				osg::Vec3 normalized = sunDirection;

				normalized.normalize();
				sunDirectionUniform->set(normalized);
			}

			else { sunDirectionUniform->set(osg::Vec3(0.0f, 0.0f, -1.0f)); }
		}
	}, osgx::imgui::SectionOptions::create(false, true));

	// Everything below is ImGui-only PRESENTATION metadata (label, slider range) layered onto the
	// uniforms hatchDefaults already created+registered above - getUniform() looks the same
	// osg::Uniform* back up by name rather than re-creating it, so a build with ImGui compiled
	// out still gets fully-initialized uniforms without ever constructing this metadata at all.
	struct HatchParam {
		osg::Uniform* uniform;
		const char* label;
		float lo, hi;
	};

	auto param = [lightingSS](const char* name, const char* label, float lo, float hi) {
		return HatchParam{lightingSS->getUniform(name), label, lo, hi};
	};

	std::vector<HatchParam> strokeParams = {
		param("warpAmount", "Warp Amount", 0.0f, 3.0f),
		param("rowJitter", "Row Jitter", 0.0f, 1.0f),
		param("widthJitter", "Width Jitter", 0.0f, 1.0f),
		param("breakLow", "Break Threshold Low", 0.0f, 1.0f),
		param("breakHigh", "Break Threshold High", 0.0f, 1.0f),
		param("nibLow", "Nib Threshold Low", 0.0f, 1.0f),
		param("nibHigh", "Nib Threshold High", 0.0f, 1.0f),
		param("hatchAA", "Antialias Floor", 0.01f, 0.3f),
	};

	std::vector<HatchParam> gateParams = {
		param("metallicSuppress", "Metallic Suppression", 0.0f, 1.0f),
		param("aoEraseLow", "AO Erase Low", 0.0f, 1.0f),
		param("aoEraseHigh", "AO Erase High", 0.0f, 1.0f),
	};

	auto familyParams = [&param](const char* prefix) {
		auto name = [prefix](const char* suffix) { return std::string(prefix) + suffix; };

		return std::vector<HatchParam>{
			param(name("Angle").c_str(), "Angle", -90.0f, 90.0f),
			param(name("SpacingScale").c_str(), "Spacing Scale", 0.2f, 3.0f),
			param(name("ThicknessScale").c_str(), "Thickness Scale", 0.2f, 3.0f),
			param(name("Seed").c_str(), "Seed", 0.0f, 100.0f),
			param(name("OnsetLow").c_str(), "Onset Low", 0.0f, 1.0f),
			param(name("OnsetHigh").c_str(), "Onset High", 0.0f, 1.0f),
		};
	};

	std::vector<HatchParam> h1Params = familyParams("h1");
	std::vector<HatchParam> h2Params = familyParams("h2");
	std::vector<HatchParam> h3Params = familyParams("h3");

	// Generic slider-block draw function shared by every HatchParam table above - reads the
	// CURRENT uniform value each frame rather than tracking a parallel local float (same
	// "osg::Uniform IS the state" shape the SSAO section below already uses), so this stays
	// correct even if something else pushes a value into one of these uniforms independently.
	// `##scope` suffixes avoid ImGui label collisions between e.g. h1/h2/h3's identical "Angle"
	// labels (see osgx.imgui's own section/label collision gotcha, ported to this Python-free
	// panel by convention).
	auto drawHatchParams = [](const std::vector<HatchParam>& params, const char* scope) {
		for(const auto& p : params) {
			float v = 0.0f;

			p.uniform->get(v);

			if(ImGui::SliderFloat((std::string(p.label) + "##" + scope).c_str(), &v, p.lo, p.hi)) {
				p.uniform->set(v);
			}
		}
	};

	gui->addSection("Hatch Stroke", [drawHatchParams, strokeParams](osg::RenderInfo&) {
		drawHatchParams(strokeParams, "stroke");
	}, osgx::imgui::SectionOptions::create(false, true));

	gui->addSection("Hatch Gate", [drawHatchParams, gateParams](osg::RenderInfo&) {
		drawHatchParams(gateParams, "gate");
	}, osgx::imgui::SectionOptions::create(false, true));

	gui->addSection("Hatch Family 1", [drawHatchParams, h1Params](osg::RenderInfo&) {
		drawHatchParams(h1Params, "h1");
	}, osgx::imgui::SectionOptions::create(false, false));

	gui->addSection("Hatch Family 2", [drawHatchParams, h2Params](osg::RenderInfo&) {
		drawHatchParams(h2Params, "h2");
	}, osgx::imgui::SectionOptions::create(false, false));

	gui->addSection("Hatch Family 3", [drawHatchParams, h3Params](osg::RenderInfo&) {
		drawHatchParams(h3Params, "h3");
	}, osgx::imgui::SectionOptions::create(false, false));

	// Live radius/bias tuning - same shape osgx-gbuffer.cpp's own "SSAO" section uses, reading
	// the CURRENT uniform value each frame rather than tracking a separate local float, since
	// osgx::SSAO::create() already returns these as real osg::Uniform*s meant to be set at any
	// time (no pass rebuild).
	gui->addSection("SSAO", [ssao](osg::RenderInfo&) {
		float radius = 0.0f, bias = 0.0f;

		ssao.radius->get(radius);
		ssao.bias->get(bias);

		bool changed = false;

		changed |= ImGui::SliderFloat("Radius", &radius, 0.01f, 2.0f);
		changed |= ImGui::SliderFloat("Bias", &bias, 0.0f, 0.1f);

		if(changed) {
			ssao.radius->set(radius);
			ssao.bias->set(bias);
		}
	}, osgx::imgui::SectionOptions::create(false, true));
#endif

	return viewer.run();
}
