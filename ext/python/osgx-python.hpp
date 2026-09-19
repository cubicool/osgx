#pragma once

// Shared framework header for the split osgx Python bindings (see osgx.cpp for the
// PYBIND11_MODULE entry point). It deliberately includes no osgx API headers: each osgx-*.cpp
// owns precisely the headers for the API it binds, so a public-header edit recompiles only its
// corresponding binding translation unit rather than every Python submodule. Splitting was a
// straight build-time win: previously any change anywhere in the single ~1700-line
// ext/osgx-python.cpp forced a full serial recompile; now each bind_*() lives in its own
// translation unit, so touching one doesn't force the others to recompile, and a from-scratch
// build parallelizes across `make -j#`.

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/CopyOp>
#include <osg/Object>

OSGX_ENABLE_WARNINGS

#include "pyosg/pyosg.hpp"

#include "pybind11x-osg.hpp"

#include <pybind11/functional.h>
#include <pybind11/stl.h>

namespace py = pybind11;
namespace pyx = pybind11x;

namespace osgx_python {

void bind_core(py::module_& m);
void bind_callbacks(py::module_& m);
void bind_cursor(py::module_& m);
void bind_rtt(py::module_& m);
void bind_pbr(py::module_& m);
void bind_shadow(py::module_& m);
void bind_gbuffer(py::module_& m);
void bind_ibl(py::module_& m);
void bind_aura(py::module_& m);
void bind_debug(py::module_& m_debug);
void bind_picking(py::module_& m);
void bind_shapes(py::module_& m);
void bind_gizmos(py::module_& m);
void bind_pixel_text(py::module_& m);
void bind_projection(py::module_& m);
void bind_sdf(py::module_& m);

#ifdef OSGX_IMGUI
void bind_imgui(py::module_& m_imgui);
#endif

#ifdef OSGX_PLATFORM
void bind_platform(py::module_& m_platform);
#endif

#ifdef OSGX_GLTF
void bind_gltf(py::module_& m_gltf);
#endif

}
