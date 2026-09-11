#include "osgx-python.hpp"
#include "osgx/RTT.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Geometry>
#include <osg/Texture>

OSGX_ENABLE_WARNINGS

// osgx::RTT IS-A osg::Camera, so its Python constructor should chain into Camera's own full
// kwargs_init_own (clearColor, renderOrder, viewport, attach-relevant texture setup, children,
// name, ... all the way up through Transform/Group/Node/Object) exactly the way osg.Camera(...)
// already does -- see pyosg/osg/Camera.cpp's own py::init(pyx::kwargs_ctor<osg::Camera>()) for the
// precedent this mirrors. RTT has nothing extra of its own to contribute (width/height/
// referenceFrame are constructor-only, not re-settable properties), so no kwargs_init_own<RTT>
// specialization is needed -- the generic no-op primary template covers it.
//
// This calls into osg::Camera's own kwargs_init_own<T> chain, a real, non-inline function body
// that only exists compiled into OpenSceneGraph.py's own libOpenSceneGraph_python{d}.a --
// osgx_python_bindings MANDATORILY links it (CMakeLists.txt: an in-tree CMake target in the
// subsumed/wheel build, or OSGX_OPENSCENEGRAPH_PY_BUILD_DIR standalone) for exactly this reason.
// See TODO.md's "osgx.RTT Python kwargs support" section.
namespace pybind11x {
template<> struct kwargs_base<osgx::RTT> { using type = osg::Camera; };
}

namespace osgx_python {

void bind_rtt(py::module_& m) {
	// Cross-module base-class inheritance: osg::Camera is bound in pyosg/osg/Camera.cpp, not here.
	// Works because osgx.cpp's py::module_::import("OpenSceneGraph") forces that registration
	// before any osgx py::class_<> that derives from an OSG type runs -- see osgx-pbr.cpp's
	// Material binding for the same pattern and its fuller explanation.
	py::class_<osgx::RTT, osg::Camera, osg::ref_ptr<osgx::RTT>>(
		m,
		"RTT",
		"A single render-to-texture osg.Camera. Every hand-rolled RTT camera used to repeat the "
		"same four lines -- PRE_RENDER, FRAME_BUFFER_OBJECT, ABSOLUTE_RF, and a viewport matching "
		"the target texture's size -- and forgetting ABSOLUTE_RF was a SILENT failure. RTT's "
		"constructor sets all four so this can no longer be forgotten at a call site. IS-A "
		"osg.Camera, not a wrapping object: every osg.Camera method (viewMatrix, projectionMatrix, "
		"clearColor, attach(), ...) stays directly available."
	)
		.def(
			py::init<int, int, osg::Transform::ReferenceFrame>(),
			"width"_a,
			"height"_a,
			"referenceFrame"_a=osg::Transform::ABSOLUTE_RF,
			"width/height: FBO resolution in pixels. referenceFrame=RELATIVE_RF is a real second "
			"shape, not just an escape hatch: a camera that never sets its own view/projection and "
			"instead inherits whatever the cull traversal's current matrices are at its position in "
			"the scene graph -- osgx.Aura's selectionCamera is exactly this. Every other camera "
			"setting (clearMask/clearColor/view/projection/attach) is left to the caller, same as a "
			"raw osg.Camera."
		)
		.def(
			py::init(
				pyx::kwargs_ctor<osgx::RTT, int, int, osg::Transform::ReferenceFrame>()
			),
			"width"_a,
			"height"_a,
			"referenceFrame"_a=osg::Transform::ABSOLUTE_RF,
			"Same as the plain constructor above, but any additional keyword arguments are applied "
			"via osg.Camera's own kwargs handling (and everything it chains into -- Transform/Group/"
			"Node/Object) after construction -- e.g. "
			"osgx.RTT(512, 512, clearColor=osg.Vec4(0, 0, 0, 1), name=\"MyRTT\")."
		)
		.def(
			"attach",
			// &osgx::RTT::attach alone is ambiguous -- `using osg::Camera::attach;` (RTT.hpp) brings
			// several base overloads into this same name, so pybind11 can't deduce which one from an
			// unqualified pointer-to-member-function. Disambiguate with an explicit cast.
			static_cast<void (osgx::RTT::*)(const osgx::RTT::AttachmentList&)>(&osgx::RTT::attach),
			"attachments"_a,
			"Declarative multi-attachment setup: a list of (component, texture) pairs, attached in "
			"order in one call -- e.g. "
			"rtt.attach([(osg.Camera.COLOR_BUFFER0, tex), (osg.Camera.DEPTH_BUFFER, depthTex)]). "
			"AttachmentList is a std::vector (matches osgx.HookList's own shape exactly), so "
			"pybind11/stl.h's vector/pair casters bind it directly -- no wrapper needed."
		)
		.def_static(
			"fullscreenQuad",
			&osgx::RTT::fullscreenQuad,
			"width"_a,
			"height"_a,
			"fragmentShaderSrc"_a,
			"Builds the GGXPrefilter/SSAO-shaped fullscreen post-process pass: identity view/"
			"projection, a full-viewport NDC quad already added as this camera's own child "
			"(osgx.FULLSCREEN_VERT + fragmentShaderSrc), depth test and cull face forced off. "
			"Baking this in avoids the silent all-fragments-discarded failure a fullscreen pass "
			"hits once retargeted from the backbuffer to an FBO with depth test left on."
		)
		.def_property(
			"quad", &osgx::RTT::getQuad, &osgx::RTT::setQuad,
			"The fullscreenQuad() drawable, for a caller that wants to inspect/replace it (e.g. "
			"custom UV layout). Already this camera's own child either way."
		)
	;
}

}
