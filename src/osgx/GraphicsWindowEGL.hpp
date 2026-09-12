#pragma once

#include "Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GraphicsContext>
#include <osgViewer/GraphicsWindow>

OSGX_ENABLE_WARNINGS

namespace osgx::platform {

// Creates an X11 window driven by EGL instead of GLX. Skeleton/proof of concept - no
// input/resize handling yet, just enough to prove an EGL-backed osgViewer::GraphicsWindow can be
// created and attached to a Camera. checkEvents() DOES watch for a clean window-manager close
// (WM_DELETE_WINDOW), so clicking the window's close button shuts the viewer down properly instead
// of leaving a dead EGL surface behind. This declaration always exists; the implementation is
// only compiled into libosgx when OSGX_EGL is defined (see OSGX_WITH_EGL in CMakeLists.txt) --
// calling this without OSGX_EGL enabled fails to link.
osg::ref_ptr<osgViewer::GraphicsWindow> createEGLWindow(osg::GraphicsContext::Traits* traits);

}
