#pragma once

#include "Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GraphicsContext>
#include <osgViewer/GraphicsWindow>

OSGX_ENABLE_WARNINGS

namespace osgx::platform {

// Direct-scanout (no X11, no window manager) DRM/KMS + GBM window, kiosk/embedded-style.
// Skeleton/proof of concept - requires exclusive access to a DRM master node
// (/dev/dri/cardN), so it will fail to initialize if an X server or Wayland compositor already
// holds the display; not testable inside a nested X11 session. This declaration always exists;
// the implementation is only compiled into libosgx when OSGX_GBM is defined (see OSGX_WITH_GBM
// in CMakeLists.txt) - calling this without OSGX_GBM enabled fails to link.
osg::ref_ptr<osgViewer::GraphicsWindow> createGBMWindow(osg::GraphicsContext::Traits* traits);

}
