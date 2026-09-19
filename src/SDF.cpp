#include "osgx/SDF.hpp"
#include "osgx/Shader.hpp"

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

}

void registerSDFShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"SHAPES", "osgx_SDF_Circle", SDF_SHAPES_SRC}
	};

	registerShaderLibs("osgx::sdf", libs);
}

}
