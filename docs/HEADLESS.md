# `osgx::headless` - offscreen rendering and capture

`osgx::headless` turns an ordinary `osgViewer::Viewer` into a deterministic offscreen render: it
creates a pbuffer, renders a fixed number of frames single-threaded, reads the final framebuffer,
writes an image, and exits. It is intended for example smoke tests, CI captures, and agent-driven
visual verification without changing the scene graph itself.

On Linux, automatic selection prefers osgX's EGL pbuffer when osgX was built with `OSGX_WITH_EGL`.
That path uses `EGL_EXT_platform_device` when available and does not require X11 or `DISPLAY`.
Otherwise it uses OSG's native pbuffer implementation: WGL on Windows, Cocoa on macOS, and GLX on
Linux (where an X display is required).

## C++

Include `osgx/Headless.hpp`, consume the headless arguments before constructing the viewer, then
replace the example's final `viewer.run()` with `osgx::headless::run()`:

```cpp
#include <osg/ArgumentParser>
#include <osgViewer/Viewer>

#include "osgx/Headless.hpp"

int main(int argc, char** argv) {
	osg::ArgumentParser arguments(&argc, argv);
	auto headless = osgx::headless::readArguments(arguments);

	osgViewer::Viewer viewer(arguments);
	viewer.setSceneData(makeScene());

	return osgx::headless::run(viewer, headless);
}
```

Call `run()` before `viewer.realize()`. When headless mode is disabled, it simply calls
`viewer.run()`, so the same executable retains its normal interactive behavior.

`readArguments()` consumes these options, so later application parsing and `osgViewer::Viewer`
never see them:

```text
--headless [egl|native]
--headless-frames N
--headless-out FILE
```

With no backend argument, `--headless` uses automatic selection. `auto`, `glx`, `wgl`, and `cocoa`
are accepted aliases. The default is ten frames and an output named
`<executable-stem>-headless.png` in the working directory.

The final capture runs as the master camera's final-draw callback, after nested and post-render
cameras. Existing final-draw callbacks run first and are restored after capture. The helper prints
the GL vendor, renderer, and version; it returns failure when context creation, capture, or image
writing fails, and warns when every captured pixel is identical.

For applications that need only a context, `createContext(width, height, samples, backend)` returns
a `Context` containing the valid `osg::GraphicsContext` and the backend actually selected. Assign
`context.graphicsContext` to a camera and configure its viewport/projection before realizing the
viewer. `samples=std::nullopt` inherits `osg::DisplaySettings`; zero disables MSAA.

## Python

The Python module exposes the context-creation portion as `osgx.headless`:

```python
from OpenSceneGraph import osg
import osgx

context = osgx.headless.createContext(
	800,
	600,
	backend=osgx.headless.Backend.Auto,
)

camera.graphicsContext = context.graphicsContext
camera.viewport = (0, 0, 800, 600)
print(context.backend)
```

`Backend.EGL` requires an osgX build with `OSGX_WITH_EGL`; `Backend.Native` requires the platform
native pbuffer path. `Backend.Auto` prefers EGL and falls back to native. `samples=None` inherits
OSG's display settings, `samples=0` disables MSAA, and a positive integer requests that sample
count.

Python-specific command-line handling and the `osgViewer.Viewer` replacement used by
OpenSceneGraph.py remain separate; this binding gives that layer the same pbuffer/backend-selection
implementation as C++.
