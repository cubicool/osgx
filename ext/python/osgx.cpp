// PYBIND11_MODULE entry point for the osgx Python module. Deliberately thin: every actual
// binding lives in its own osgx-*.cpp translation unit (see osgx-python.hpp for the bind_*
// declarations), each compiled into the osgx_python_bindings static library (CMakeLists.txt).
// This file just wires the submodules together, which keeps its own compile time (and therefore
// the cost of touching it) trivial, and means a change to one binding group no longer forces a
// full serial recompile of everything else.
#include "osgx-python.hpp"
#include "osgx/Version.hpp"

PYBIND11_MODULE(osgx, m) {
	// Side effect, not the value: forces the OpenSceneGraph module's pybind11 types (osg::Geometry,
	// osgGA::CameraManipulator, etc.) to be registered before any osgx py::class_<> that derives
	// from one of them runs below.
	py::module_::import("OpenSceneGraph");

	osgx_python::bind_library(m);
	osgx_python::bind_core(m);
	osgx_python::bind_callbacks(m);
	osgx_python::bind_rtt(m);

	// pbr/shadow/gbuffer/ibl/picking/cursor used to live in their own osgx::{foo}:: C++
	// namespaces; those collapsed into plain osgx:: this week, so their bindings now go straight
	// onto the top-level module too instead of a submodule that no longer corresponds to anything
	// in C++. bind_cursor() specifically used to live under osgx.platform (osgx::Cursor.hpp never
	// actually needed anything X11-specific) - see ext/python/osgx-cursor.cpp.
	osgx_python::bind_cursor(m);
	osgx_python::bind_pbr(m);
	osgx_python::bind_light(m);
	osgx_python::bind_shadow(m);
	osgx_python::bind_gbuffer(m);
	osgx_python::bind_ibl(m);
	osgx_python::bind_environment(m);
	osgx_python::bind_aura(m);

	auto m_debug = m.def_submodule("debug", "osgx::debug - GL_KHR_debug integration + GPU/CPU profiler");

	osgx_python::bind_debug(m_debug);

#ifdef OSGX_IMGUI
	auto m_imgui = m.def_submodule("imgui", "osgx::imgui namespace");

	osgx_python::bind_imgui(m_imgui);
#endif

#ifdef OSGX_PLATFORM
	auto m_platform = m.def_submodule("platform", "osgx::platform - Linux/X11 window helpers");

	osgx_python::bind_platform(m_platform);
#endif

	osgx_python::bind_picking(m);

	osgx_python::bind_shapes(m);
	osgx_python::bind_gizmos(m);
	osgx_python::bind_pixel_text(m);
	osgx_python::bind_projection(m);
	osgx_python::bind_sdf(m);

#ifdef OSGX_GLTF
	auto m_gltf = m.def_submodule("gltf", "osgx::gltf - glTF 2.0 loader + optional PBR/IBL adapter");

	osgx_python::bind_gltf(m_gltf);
#endif

	py::dict info;

	// A plain version string, matching OpenSceneGraph.py's build_info()["version"] and this
	// same dict's own "osg" key - was a (major, minor, patch) tuple, the only non-string value
	// build_info() ever returned.
	info["version"] = OSGX_VERSION;

	pyx::build_info(m, info);
}
