#include "osgx/GraphicsWindowEGL.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Notify>
#include <osg/State>
#include <osgViewer/GraphicsWindow>

OSGX_ENABLE_WARNINGS

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <X11/Xlib.h>

#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace osgx::platform {

namespace {

// Opens a bare X11 window (no WM decoration/events beyond mapping) for EGL to drive via
// eglCreateWindowSurface().
std::pair<Display*, Window> createEGLDisplayWindow(unsigned int width, unsigned int height) {
	Display* display = XOpenDisplay(nullptr);

	if(!display) {
		osg::notify(osg::FATAL) << "EGL: XOpenDisplay failed" << std::endl;

		return {nullptr, 0};
	}

	Window root = DefaultRootWindow(display);
	Window win = XCreateSimpleWindow(display, root, 0, 0, width, height, 0, 0, 0);

	XMapWindow(display, win);
	XStoreName(display, win, "OSG EGL Window");

	return {display, win};
}

// Headless EGL displays are per-device singletons shared by every pbuffer context on that
// device, and eglTerminate() is not reference counted: terminating on one context's close would
// invalidate every other live context. Each acquire is paired with one release; the display is
// terminated only when its last user releases it.
std::mutex headlessMutex;
std::map<EGLDisplay, unsigned int> headlessRefs;

EGLDisplay initializeHeadlessDisplay(EGLint& major, EGLint& minor);

EGLDisplay acquireHeadlessDisplay(EGLint& major, EGLint& minor) {
	std::lock_guard<std::mutex> lock(headlessMutex);

	EGLDisplay display = initializeHeadlessDisplay(major, minor);

	if(display != EGL_NO_DISPLAY) headlessRefs[display]++;

	return display;
}

void releaseHeadlessDisplay(EGLDisplay display) {
	std::lock_guard<std::mutex> lock(headlessMutex);

	auto it = headlessRefs.find(display);

	if(it == headlessRefs.end()) return;

	if(--it->second == 0) {
		headlessRefs.erase(it);

		eglTerminate(display);
	}
}

// Returns an initialized EGL display that needs no X server: the first EGL device (via
// EGL_EXT_platform_device) that initializes, else EGL_DEFAULT_DISPLAY. eglInitialize() on an
// already-initialized display is a no-op, so repeated calls return the same live display.
EGLDisplay initializeHeadlessDisplay(EGLint& major, EGLint& minor) {
	auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(
		eglGetProcAddress("eglQueryDevicesEXT")
	);

	auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
		eglGetProcAddress("eglGetPlatformDisplayEXT")
	);

	if(queryDevices && getPlatformDisplay) {
		EGLDeviceEXT devices[16];
		EGLint numDevices = 0;

		if(queryDevices(16, devices, &numDevices)) {
			for(EGLint i = 0; i < numDevices; i++) {
				EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);

				if(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor)) return display;
			}
		}
	}

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

	if(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor)) return display;

	return EGL_NO_DISPLAY;
}

class GraphicsWindowEGL: public osgViewer::GraphicsWindow {
public:
	explicit GraphicsWindowEGL(osg::GraphicsContext::Traits* traits) {
		_traits = traits;

		init();
	}

	~GraphicsWindowEGL() override {
		close(true);
	}

	bool valid() const override { return _valid; }

	void init() {
		if(_initialized) return;

		if(!_traits) {
			osg::notify(osg::FATAL) << "EGL: no traits" << std::endl;

			return;
		}

		_traits->windowDecoration = false;

		// A pbuffer surface is single-buffered; eglSwapBuffers() on it is a no-op.
		if(_traits->pbuffer) _traits->doubleBuffer = false;

		if(_traits->pbuffer) {
			_eglDisplay = acquireHeadlessDisplay(_eglMajor, _eglMinor);
			_headless = _eglDisplay != EGL_NO_DISPLAY;

			if(_eglDisplay == EGL_NO_DISPLAY) {
				osg::notify(osg::FATAL) << "EGL: no headless display could be initialized" << std::endl;

				return;
			}
		}

		else {
			auto [display, win] = createEGLDisplayWindow(
				static_cast<unsigned int>(_traits->width),
				static_cast<unsigned int>(_traits->height)
			);

			if(!display) return;

			_display = display;
			_window = win;
			_wmDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);

			XSetWMProtocols(display, win, &_wmDeleteWindow, 1);

			_eglDisplay = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display));

			if(_eglDisplay == EGL_NO_DISPLAY) {
				osg::notify(osg::FATAL) << "EGL: eglGetDisplay failed" << std::endl;

				return;
			}

			if(!eglInitialize(_eglDisplay, &_eglMajor, &_eglMinor)) {
				osg::notify(osg::FATAL) << "EGL: eglInitialize failed" << std::endl;

				return;
			}
		}

		if(!eglBindAPI(EGL_OPENGL_API)) {
			osg::notify(osg::FATAL) << "EGL: eglBindAPI(EGL_OPENGL_API) failed" << std::endl;

			return;
		}

		std::vector<EGLint> configAttribs = {
			EGL_SURFACE_TYPE, _traits->pbuffer ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_DEPTH_SIZE, 24
		};

		if(_traits->sampleBuffers) {
			configAttribs.push_back(EGL_SAMPLE_BUFFERS);
			configAttribs.push_back(static_cast<EGLint>(_traits->sampleBuffers));
		}

		if(_traits->samples) {
			configAttribs.push_back(EGL_SAMPLES);
			configAttribs.push_back(static_cast<EGLint>(_traits->samples));
		}

		configAttribs.push_back(EGL_NONE);

		EGLint numConfigs = 0;

		if(
			!eglChooseConfig(_eglDisplay, configAttribs.data(), &_eglConfig, 1, &numConfigs) ||
			numConfigs < 1
		) {
			osg::notify(osg::FATAL) << "EGL: eglChooseConfig failed" << std::endl;

			return;
		}

		if(_traits->pbuffer) {
			const EGLint pbufferAttribs[] = {
				EGL_WIDTH, _traits->width,
				EGL_HEIGHT, _traits->height,
				EGL_NONE
			};

			_eglSurface = eglCreatePbufferSurface(_eglDisplay, _eglConfig, pbufferAttribs);
		}

		else _eglSurface = eglCreateWindowSurface(
			_eglDisplay,
			_eglConfig,
			reinterpret_cast<EGLNativeWindowType>(_window),
			nullptr
		);

		if(_eglSurface == EGL_NO_SURFACE) {
			osg::notify(osg::FATAL) << "EGL: surface creation failed" << std::endl;

			return;
		}

		// Mirrors osgViewer's GLX_ARB_create_context path: the version is always requested (OSG's
		// default "1.0" yields the driver's highest compatibility context), the profile mask only
		// for 3.2+, and the flags only when set. EGL_KHR_create_context uses the same flag and
		// profile bit values as GLX_ARB_create_context, so Traits values pass through unchanged.
		std::vector<EGLint> contextAttribs;

		unsigned int major = 0;
		unsigned int minor = 0;

		if(_traits->getContextVersion(major, minor)) {
			contextAttribs.push_back(EGL_CONTEXT_MAJOR_VERSION);
			contextAttribs.push_back(static_cast<EGLint>(major));
			contextAttribs.push_back(EGL_CONTEXT_MINOR_VERSION);
			contextAttribs.push_back(static_cast<EGLint>(minor));

			if((major > 3 || (major == 3 && minor >= 2)) && _traits->glContextProfileMask) {
				contextAttribs.push_back(EGL_CONTEXT_OPENGL_PROFILE_MASK);
				contextAttribs.push_back(static_cast<EGLint>(_traits->glContextProfileMask));
			}
		}

		if(_traits->glContextFlags) {
			contextAttribs.push_back(EGL_CONTEXT_FLAGS_KHR);
			contextAttribs.push_back(static_cast<EGLint>(_traits->glContextFlags));
		}

		contextAttribs.push_back(EGL_NONE);

		_eglContext = eglCreateContext(_eglDisplay, _eglConfig, EGL_NO_CONTEXT, contextAttribs.data());

		if(_eglContext == EGL_NO_CONTEXT) {
			osg::notify(osg::FATAL) << "EGL: eglCreateContext failed" << std::endl;

			return;
		}

		if(!eglMakeCurrent(_eglDisplay, _eglSurface, _eglSurface, _eglContext)) {
			osg::notify(osg::FATAL) << "EGL: initial eglMakeCurrent failed" << std::endl;

			return;
		}

		osg::ref_ptr<osg::State> state = new osg::State();

		state->setGraphicsContext(this);
		state->setContextID(osg::GraphicsContext::createNewContextID());

		setState(state);

		_initialized = true;
		_realized = true;
		_valid = true;

		osg::notify(osg::NOTICE)
			<< "EGL initialized: " << _eglMajor << "." << _eglMinor
			<< " vendor=" << eglQueryString(_eglDisplay, EGL_VENDOR)
			<< (_traits->pbuffer ? " pbuffer=" : " surface=")
			<< _traits->width << "x" << _traits->height
			<< std::endl
		;
	}

	bool realizeImplementation() override {
		if(!_initialized) init();

		_realized = _valid;

		return _realized;
	}

	bool isRealizedImplementation() const override { return _realized; }

	// Watches for the WM_DELETE_WINDOW ClientMessage the window manager sends when the user
	// clicks the close button (registered via XSetWMProtocols() in init()). Without this, closing
	// the window kills it out from under EGL, and every subsequent makeCurrentImplementation()
	// fails forever since nothing ever tells the viewer the window (and thus the GraphicsContext)
	// is gone.
	bool checkEvents() override {
		while(_display && XPending(_display)) {
			XEvent event;

			XNextEvent(_display, &event);

			if(event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == _wmDeleteWindow) {
				getEventQueue()->closeWindow();
			}
		}

		return osgViewer::GraphicsWindow::checkEvents();
	}

	bool makeCurrentImplementation() override {
		bool ok = eglMakeCurrent(_eglDisplay, _eglSurface, _eglSurface, _eglContext) == EGL_TRUE;

		if(!ok) osg::notify(osg::FATAL) << "EGL: eglMakeCurrent failed" << std::endl;

		return ok;
	}

	bool releaseContextImplementation() override {
		return eglMakeCurrent(_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
	}

	void swapBuffersImplementation() override {
		eglSwapBuffers(_eglDisplay, _eglSurface);
	}

	void closeImplementation() override {
		if(_eglDisplay != EGL_NO_DISPLAY) {
			if(_eglContext != EGL_NO_CONTEXT) eglDestroyContext(_eglDisplay, _eglContext);
			if(_eglSurface != EGL_NO_SURFACE) eglDestroySurface(_eglDisplay, _eglSurface);

			if(_headless) releaseHeadlessDisplay(_eglDisplay);

			else eglTerminate(_eglDisplay);
		}

		_headless = false;

		if(_display) {
			if(_window) XDestroyWindow(_display, _window);

			XCloseDisplay(_display);
		}

		_eglDisplay = EGL_NO_DISPLAY;
		_eglContext = EGL_NO_CONTEXT;
		_eglSurface = EGL_NO_SURFACE;
		_eglConfig = nullptr;

		_display = nullptr;
		_window = 0;

		_initialized = false;
		_realized = false;
		_valid = false;
	}

private:
	bool _valid = false;
	bool _initialized = false;
	bool _realized = false;
	bool _headless = false;

	Display* _display = nullptr;
	Window _window = 0;
	Atom _wmDeleteWindow = None;

	EGLDisplay _eglDisplay = EGL_NO_DISPLAY;
	EGLContext _eglContext = EGL_NO_CONTEXT;
	EGLSurface _eglSurface = EGL_NO_SURFACE;
	EGLConfig _eglConfig = nullptr;

	EGLint _eglMajor = 0;
	EGLint _eglMinor = 0;
};

}

osg::ref_ptr<osgViewer::GraphicsWindow> createEGLWindow(osg::GraphicsContext::Traits* traits) {
	return new GraphicsWindowEGL(traits);
}

}
