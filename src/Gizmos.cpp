#include "osgx/Gizmos.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BoundingSphere>
#include <osg/Geode>
#include <osg/LineWidth>
#include <osg/Math>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Vec2>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cmath>

namespace osgx {

namespace detail {

constexpr int CIRCLE_SEGMENTS = 24;
constexpr int SPOT_RING_SEGMENTS = 24;
constexpr int SPOT_SPOKES = 8;
// Per-light vertex budget: whichever shape needs more (three wireframe circles for a point/sphere
// marker) sets the fixed per-slot capacity every light gets, so the backing arrays never change
// SIZE across a rebuild - only their contents - letting OSG upload via glBufferSubData instead
// of reallocating each frame (same lesson 11-sketchfab-lambertian.py's LightGizmoCallback comment
// records: a new Array object/size every frame forces a full glBufferData reallocation).
constexpr int MAX_VERTS_PER_LIGHT = 3 * CIRCLE_SEGMENTS;

constexpr const char GIZMO_VERTEX_SHADER[] = R"GLSL(
#version 460 core

in vec4 osgx_gizmo_Vertex;
in vec3 osgx_gizmo_Color;

uniform mat4 osg_ModelViewProjectionMatrix;

out vec3 vColor;

void main() {
	vColor = osgx_gizmo_Color;
	gl_Position = osg_ModelViewProjectionMatrix * osgx_gizmo_Vertex;
}
)GLSL";

constexpr const char GIZMO_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core

in vec3 vColor;

out vec4 fragColor;

void main() {
	fragColor = vec4(vColor, 1.0);
}
)GLSL";

osg::ref_ptr<osg::Program> createGizmoProgram() {
	auto program = osgx::make_nref<osg::Program>("osgx_gizmo");

	program->addShader(new osg::Shader(osg::Shader::VERTEX, GIZMO_VERTEX_SHADER));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, GIZMO_FRAGMENT_SHADER));
	program->addBindAttribLocation("osgx_gizmo_Vertex", 0);
	program->addBindAttribLocation("osgx_gizmo_Color", 1);

	return program;
}

// Normalizes by the largest channel, matching 11-sketchfab-lambertian.py's LightGizmoCallback --
// a gizmo drawn at a light's raw HDR color (routinely >> 1.0) would just be solid white.
osg::Vec3 gizmoColor(const osg::Vec3& color) {
	float m = std::max({color.x(), color.y(), color.z(), 1e-4f});

	return color / m;
}

// Builds an orthonormal basis (u, v) perpendicular to `n`, same construction as
// 11-sketchfab-lambertian.py's LightGizmoCallback: a Z-up reference vector, except when `n` is
// nearly parallel to it, where X is used instead to avoid a degenerate/zero-length cross.
void perpendicularBasis(const osg::Vec3& n, osg::Vec3& u, osg::Vec3& v) {
	osg::Vec3 upRef = std::abs(n.z()) > 0.99f ? osg::Vec3(1.0f, 0.0f, 0.0f) : osg::Vec3(0.0f, 0.0f, 1.0f);

	u = upRef ^ n;
	u.normalize();
	v = n ^ u;
}

// Appends a wireframe circle (verts.size() vertices, drawn as a LINE_LOOP primitive set covering
// [start, start+verts.size())) centered at `center` in the plane spanned by `u`/`v`. `verts`/
// `colors` are already the caller's sub-span for this circle - indices here are span-local, only
// the primitive-set's GL start offset needs the absolute `start`.
void appendCircle(
	std::span<osg::Vec3> verts,
	std::span<osg::Vec3> colors,
	osg::Geometry* geom,
	std::size_t start,
	const osg::Vec3& center,
	const osg::Vec3& u,
	const osg::Vec3& v,
	float radius,
	const osg::Vec3& color
) {
	auto segments = static_cast<int>(verts.size());

	for(int i = 0; i < segments; i++) {
		float a = 2.0f * osg::PIf * static_cast<float>(i) / static_cast<float>(segments);
		auto idx = static_cast<std::size_t>(i);

		verts[idx] = center + u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
		colors[idx] = color;
	}

	geom->addPrimitiveSet(new osg::DrawArrays(GL_LINE_LOOP, static_cast<GLint>(start), segments));
}

// Point/sphere marker: three orthogonal wireframe circles (XY/XZ/YZ great circles) at `position`,
// sized to `radius`. Consumes exactly 3*CIRCLE_SEGMENTS vertices starting at `start`.
void buildPointMarker(
	osgx::Vec3Array* verts,
	osgx::Vec3Array* colors,
	osg::Geometry* geom,
	std::size_t start,
	const osg::Vec3& position,
	float radius,
	const osg::Vec3& color
) {
	auto segs = static_cast<std::size_t>(CIRCLE_SEGMENTS);

	appendCircle(
		verts->span(start, segs), colors->span(start, segs), geom, start,
		position, osg::Vec3(1.0f, 0.0f, 0.0f), osg::Vec3(0.0f, 1.0f, 0.0f), radius, color
	);
	appendCircle(
		verts->span(start + segs, segs), colors->span(start + segs, segs), geom, start + segs,
		position, osg::Vec3(1.0f, 0.0f, 0.0f), osg::Vec3(0.0f, 0.0f, 1.0f), radius, color
	);
	appendCircle(
		verts->span(start + 2 * segs, segs), colors->span(start + 2 * segs, segs), geom, start + 2 * segs,
		position, osg::Vec3(0.0f, 1.0f, 0.0f), osg::Vec3(0.0f, 0.0f, 1.0f), radius, color
	);
}

// Spot marker: a wireframe cone - a ring at `coneLength` along `direction` (radius from the
// outer cone half-angle) plus SPOT_SPOKES lines from the apex to that ring. Consumes at most
// MAX_VERTS_PER_LIGHT vertices starting at `start` (SPOT_RING_SEGMENTS + SPOT_SPOKES*2).
void buildSpotMarker(
	osgx::Vec3Array* verts,
	osgx::Vec3Array* colors,
	osg::Geometry* geom,
	std::size_t start,
	const osg::Vec3& apex,
	const osg::Vec3& direction,
	float outerConeAngle,
	float coneLength,
	const osg::Vec3& color
) {
	osg::Vec3 axis = direction;

	axis.normalize();

	osg::Vec3 u, v;

	perpendicularBasis(axis, u, v);

	osg::Vec3 ringCenter = apex + axis * coneLength;
	float ringRadius = coneLength * std::tan(outerConeAngle);
	auto ringSegs = static_cast<std::size_t>(SPOT_RING_SEGMENTS);

	appendCircle(verts->span(start, ringSegs), colors->span(start, ringSegs), geom, start, ringCenter, u, v, ringRadius, color);

	std::size_t spokeStart = start + ringSegs;
	int stride = std::max(1, SPOT_RING_SEGMENTS / SPOT_SPOKES);
	auto spokeCount = static_cast<std::size_t>(SPOT_SPOKES) * 2;
	auto spokeVerts = verts->span(spokeStart, spokeCount);
	auto spokeColors = colors->span(spokeStart, spokeCount);

	for(int i = 0; i < SPOT_SPOKES; i++) {
		float a = 2.0f * osg::PIf * static_cast<float>(i * stride) / static_cast<float>(SPOT_RING_SEGMENTS);
		osg::Vec3 ringPoint = ringCenter + u * (std::cos(a) * ringRadius) + v * (std::sin(a) * ringRadius);
		std::size_t base = static_cast<std::size_t>(i) * 2;

		spokeVerts[base] = apex;
		spokeColors[base] = color;
		spokeVerts[base + 1] = ringPoint;
		spokeColors[base + 1] = color;
	}

	geom->addPrimitiveSet(new osg::DrawArrays(GL_LINES, static_cast<GLint>(spokeStart), SPOT_SPOKES * 2));
}

// Builds the non-depth-tested POST_RENDER overlay camera for directional lights - see
// LightGizmos' doc comment in Gizmos.hpp for the full rationale.
osg::ref_ptr<osg::Camera> buildDirectionalOverlay(const osgx::LightSet& lights, osg::Node* scene) {
	osg::BoundingSphere bound = scene ? scene->getBound() : osg::BoundingSphere(osg::Vec3(), 1.0f);
	float boundRadius = bound.radius() > 0.0f ? bound.radius() : 1.0f;
	osg::Vec3 boundCenter = bound.center();

	// Proportional to the scene, same reasoning as 11-sketchfab-lambertian.py's
	// create_light_gizmo(): large enough to read as "an infinite plane of parallel rays" rather
	// than a small marker.
	float planeHalfSize = boundRadius * 2.5f;
	float normalLength = boundRadius * 0.8f;
	float arrowBack = normalLength * 0.2f;
	float arrowWidth = normalLength * 0.15f;

	// 10 vertices per directional slot: 4 (plane LINE_LOOP) + 2 (direction stub, LINES) + 4
	// (arrowhead, two LINES segments) - exactly mirrors the Python original's per-light layout.
	constexpr std::size_t VERTS_PER_LIGHT = 10;
	auto capacity = static_cast<std::size_t>(osgx::MAX_LIGHTS) * VERTS_PER_LIGHT;
	auto verts = osgx::make_ref<osgx::Vec3Array>(capacity);
	auto colors = osgx::make_ref<osgx::Vec3Array>(capacity);

	verts->setBinding(osg::Array::BIND_PER_VERTEX);
	colors->setBinding(osg::Array::BIND_PER_VERTEX);
	verts->setDataVariance(osg::Object::DYNAMIC);
	colors->setDataVariance(osg::Object::DYNAMIC);

	auto geom = osgx::make_ref<osg::Geometry>();

	geom->setUseVertexBufferObjects(true);
	geom->setDataVariance(osg::Object::DYNAMIC);
	geom->setVertexArray(verts);
	geom->setVertexAttribArray(0, verts);
	geom->setVertexAttribArray(1, colors);
	geom->setCullingActive(false);

	auto* geomSS = geom->getOrCreateStateSet();

	geomSS->setAttributeAndModes(createGizmoProgram(), osg::StateAttribute::ON);
	geomSS->setAttributeAndModes(new osg::LineWidth(2.0f), osg::StateAttribute::ON);
	geomSS->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
	geomSS->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(geom);

	struct DirectionalOverlayCallback: public osg::NodeCallback {
		DirectionalOverlayCallback(const osgx::LightSet& lights, osg::Vec3 center, float planeHalf, float normalLen, float back, float wide):
		_lights(const_cast<osgx::LightSet*>(&lights)), _center(center), _planeHalf(planeHalf), _normalLen(normalLen), _arrowBack(back), _arrowWidth(wide) {}

		void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
			auto* geode2 = dynamic_cast<osg::Geode*>(node);

			if(!geode2 || geode2->getNumDrawables() == 0 || !_lights.valid()) {
				traverse(node, nv);

				return;
			}

			auto* geom2 = static_cast<osg::Geometry*>(geode2->getDrawable(0));
			auto* verts2 = static_cast<osgx::Vec3Array*>(geom2->getVertexArray());
			auto* colors2 = static_cast<osgx::Vec3Array*>(geom2->getVertexAttribArray(1));

			geom2->removePrimitiveSet(0, geom2->getNumPrimitiveSets());

			for(int i = 0; i < osgx::MAX_LIGHTS; i++) {
				auto index = static_cast<std::size_t>(i);

				if(!_lights->getEnabled(index)) continue;

				osgx::LightType type = _lights->getType(index);

				if(type != osgx::LightType::Directional) continue;

				osg::Vec3 direction = _lights->getDirection(index);
				osg::Vec3 lightColorValue = _lights->getColor(index);

				// `direction` is the ray TRAVEL direction (light -> surface; see LIGHT_UNIFORMS'
				// comment in PBR.hpp and osgx_DirectionalLightRadiance's `L = -normalize(direction)`
				// - L, surface-to-light, is the negation). The plane represents the "wall of
				// parallel rays" and belongs on the light-SOURCE side of the object, i.e. offset
				// against travel direction (-direction, not +direction); the arrow then points
				// FROM that plane TOWARD the object, which is the true travel direction (+direction)
				// - both were flipped (offset by +direction, arrow pointing -direction) before this
				// fix, which put the plane on the wrong side and made the arrow point backward.
				float len = direction.length();
				osg::Vec3 n = len > 1e-6f ? -direction / len : osg::Vec3(0.0f, 0.0f, -1.0f);
				osg::Vec3 center = _center + n * _normalLen;
				osg::Vec3 u, v;

				perpendicularBasis(n, u, v);

				auto slot = verts2->span(static_cast<std::size_t>(i) * 10, 10);
				auto slotColors = colors2->span(static_cast<std::size_t>(i) * 10, 10);

				slot[0] = center + u * _planeHalf + v * _planeHalf;
				slot[1] = center - u * _planeHalf + v * _planeHalf;
				slot[2] = center - u * _planeHalf - v * _planeHalf;
				slot[3] = center + u * _planeHalf - v * _planeHalf;
				slot[4] = center;

				osg::Vec3 tip = center - n * _normalLen;

				slot[5] = tip;

				osg::Vec3 wingBase = tip + n * _arrowBack;

				slot[6] = tip;
				slot[7] = wingBase + u * _arrowWidth;
				slot[8] = tip;
				slot[9] = wingBase - u * _arrowWidth;

				osg::Vec3 c = gizmoColor(lightColorValue);

				for(auto& sc : slotColors) sc = c;

				auto start = static_cast<GLint>(static_cast<std::size_t>(i) * 10);

				geom2->addPrimitiveSet(new osg::DrawArrays(GL_LINE_LOOP, start, 4));
				geom2->addPrimitiveSet(new osg::DrawArrays(GL_LINES, start + 4, 6));
			}

			verts2->dirty();
			colors2->dirty();
			geom2->dirtyBound();

			traverse(node, nv);
		}

		osg::ref_ptr<osgx::LightSet> _lights;
		osg::Vec3 _center;
		float _planeHalf, _normalLen, _arrowBack, _arrowWidth;
	};

	geode->setUpdateCallback(
		new DirectionalOverlayCallback(lights, boundCenter, planeHalfSize, normalLength, arrowBack, arrowWidth)
	);

	auto camera = osgx::make_ref<osg::Camera>();

	camera->setReferenceFrame(osg::Transform::RELATIVE_RF);
	camera->setRenderOrder(osg::Camera::POST_RENDER);
	camera->setClearMask(0);
	camera->setAllowEventFocus(false);
	camera->addChild(geode);

	return camera;
}

}

LightMarkers::LightMarkers(const osgx::LightSet& lights, float minMarkerRadius, float spotConeLength) {
	setUpdateCallback(new UpdateCallback(lights, minMarkerRadius, spotConeLength));
	setCullingActive(false);
}

void LightMarkers::UpdateCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	if(auto* markers = dynamic_cast<LightMarkers*>(node); markers && _lights.valid())
		markers->rebuild(*_lights, _minMarkerRadius, _spotConeLength);

	traverse(node, nv);
}

void LightMarkers::rebuild(const osgx::LightSet& lights, float minMarkerRadius, float spotConeLength) {
	if(!lights.valid()) return;

	if(!_geometry) {
		auto capacity = static_cast<std::size_t>(osgx::MAX_LIGHTS) * static_cast<std::size_t>(detail::MAX_VERTS_PER_LIGHT);
		auto verts = osgx::make_ref<osgx::Vec3Array>(capacity);
		auto colors = osgx::make_ref<osgx::Vec3Array>(capacity);

		verts->setBinding(osg::Array::BIND_PER_VERTEX);
		colors->setBinding(osg::Array::BIND_PER_VERTEX);
		verts->setDataVariance(osg::Object::DYNAMIC);
		colors->setDataVariance(osg::Object::DYNAMIC);

		_geometry = osgx::make_ref<osg::Geometry>();
		_geometry->setUseVertexBufferObjects(true);
		_geometry->setDataVariance(osg::Object::DYNAMIC);
		_geometry->setVertexArray(verts);
		_geometry->setVertexAttribArray(0, verts);
		_geometry->setVertexAttribArray(1, colors);

		auto* ss2 = _geometry->getOrCreateStateSet();

		ss2->setAttributeAndModes(detail::createGizmoProgram(), osg::StateAttribute::ON);
		ss2->setAttributeAndModes(new osg::LineWidth(2.0f), osg::StateAttribute::ON);

		auto geode = osgx::make_ref<osg::Geode>();

		geode->addDrawable(_geometry);
		addChild(geode);
	}

	_geometry->removePrimitiveSet(0, _geometry->getNumPrimitiveSets());

	auto* verts = static_cast<osgx::Vec3Array*>(_geometry->getVertexArray());
	auto* colors = static_cast<osgx::Vec3Array*>(_geometry->getVertexAttribArray(1));

	for(int i = 0; i < osgx::MAX_LIGHTS; i++) {
		auto index = static_cast<std::size_t>(i);

		if(!lights.getEnabled(index)) continue;

		osgx::LightType type = lights.getType(index);
		osg::Vec4 posIntensity = lights.getPosIntensity(index);
		osg::Vec3 color = lights.getColor(index);
		float sourceRadius = lights.getSourceRadius(index);

		std::size_t slotStart = static_cast<std::size_t>(i) * static_cast<std::size_t>(detail::MAX_VERTS_PER_LIGHT);
		osg::Vec3 c = detail::gizmoColor(color);
		osg::Vec3 position(posIntensity.x(), posIntensity.y(), posIntensity.z());

		if(type == osgx::LightType::Spot) {
			osg::Vec3 direction = lights.getDirection(index);
			osg::Vec2 coneAngles = lights.getSpotAngles(index);
			float outerAngle = std::acos(std::clamp(coneAngles.y(), -1.0f, 1.0f));

			detail::buildSpotMarker(verts, colors, _geometry, slotStart, position, direction, outerAngle, spotConeLength, c);
		}

		else if(type != osgx::LightType::Directional) {
			float radius = std::max(sourceRadius, minMarkerRadius);

			detail::buildPointMarker(verts, colors, _geometry, slotStart, position, radius, c);
		}
	}

	verts->dirty();
	colors->dirty();
	_geometry->dirtyBound();
}

LightGizmos::LightGizmos(
	const osgx::LightSet& lights,
	osg::Node* scene,
	float minMarkerRadius,
	float spotConeLength
) {
	_markers = osgx::make_ref<LightMarkers>(lights, minMarkerRadius, spotConeLength);
	_overlay = detail::buildDirectionalOverlay(lights, scene);

	addChild(_markers);
	addChild(_overlay);
}

}
