#include "osgx/GraphicsWindowGBM.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Notify>
#include <osg/State>
#include <osgViewer/GraphicsWindow>

OSGX_ENABLE_WARNINGS

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace osgx::platform {

namespace {

// -----------------------------------------------------------------------------------------
// Small DRM framebuffer wrapper attached to a GBM BO.
// -----------------------------------------------------------------------------------------

struct DrmFb {
	uint32_t fbId = 0;
};

void drmFbDestroyCallback(gbm_bo* bo, void* data) {
	auto* fb = static_cast<DrmFb*>(data);

	if(!fb) return;

	int fd = gbm_device_get_fd(gbm_bo_get_device(bo));

	if(fb->fbId) drmModeRmFB(fd, fb->fbId);

	delete fb;
}

DrmFb* drmFbGetFromBo(gbm_bo* bo) {
	if(!bo) return nullptr;

	auto* fb = static_cast<DrmFb*>(gbm_bo_get_user_data(bo));

	if(fb) return fb;

	fb = new DrmFb;

	const uint32_t width = gbm_bo_get_width(bo);
	const uint32_t height = gbm_bo_get_height(bo);
	const uint32_t format = gbm_bo_get_format(bo);

	uint32_t handles[4] = {};
	uint32_t strides[4] = {};
	uint32_t offsets[4] = {};

	handles[0] = gbm_bo_get_handle(bo).u32;
	strides[0] = gbm_bo_get_stride(bo);
	offsets[0] = 0;

	int fd = gbm_device_get_fd(gbm_bo_get_device(bo));

	if(drmModeAddFB2(fd, width, height, format, handles, strides, offsets, &fb->fbId, 0) != 0) {
		if(drmModeAddFB(fd, width, height, 24, 32, strides[0], handles[0], &fb->fbId) != 0) {
			osg::notify(osg::FATAL)
				<< "GBM: drmModeAddFB2/drmModeAddFB failed: " << std::strerror(errno) << std::endl;

			delete fb;

			return nullptr;
		}
	}

	gbm_bo_set_user_data(bo, fb, drmFbDestroyCallback);

	return fb;
}

int openDrmCard() {
	char path[64];

	for(int i = 0; i < 16; i++) {
		std::snprintf(path, sizeof(path), "/dev/dri/card%d", i);

		int fd = open(path, O_RDWR | O_CLOEXEC);

		if(fd < 0) continue;

		drmModeRes* res = drmModeGetResources(fd);

		if(res) {
			drmModeFreeResources(res);

			osg::notify(osg::NOTICE) << "DRM: using " << path << std::endl;

			return fd;
		}

		close(fd);
	}

	osg::notify(osg::FATAL) << "DRM: no usable card node found in /dev/dri/card0..15" << std::endl;

	return -1;
}

uint32_t findCrtcForEncoder(drmModeRes* resources, drmModeEncoder* encoder) {
	if(encoder->crtc_id) return encoder->crtc_id;

	for(int i = 0; i < resources->count_crtcs; i++) {
		if(encoder->possible_crtcs & (1u << i)) return resources->crtcs[i];
	}

	return 0;
}

uint32_t findCrtcForConnector(int fd, drmModeRes* resources, drmModeConnector* connector) {
	if(connector->encoder_id) {
		drmModeEncoder* enc = drmModeGetEncoder(fd, connector->encoder_id);

		if(enc) {
			uint32_t crtcId = findCrtcForEncoder(resources, enc);

			drmModeFreeEncoder(enc);

			if(crtcId) return crtcId;
		}
	}

	for(int i = 0; i < connector->count_encoders; i++) {
		drmModeEncoder* enc = drmModeGetEncoder(fd, connector->encoders[i]);

		if(!enc) continue;

		uint32_t crtcId = findCrtcForEncoder(resources, enc);

		drmModeFreeEncoder(enc);

		if(crtcId) return crtcId;
	}

	return 0;
}

drmModeModeInfo chooseDrmMode(drmModeConnector* connector, unsigned int requestedW, unsigned int requestedH) {
	drmModeModeInfo best = connector->modes[0];

	if(requestedW > 0 && requestedH > 0) {
		for(int i = 0; i < connector->count_modes; i++) {
			const drmModeModeInfo& mode = connector->modes[i];

			if(
				static_cast<unsigned>(mode.hdisplay) == requestedW &&
				static_cast<unsigned>(mode.vdisplay) == requestedH
			) return mode;
		}
	}

	for(int i = 0; i < connector->count_modes; i++) {
		if(connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) return connector->modes[i];
	}

	return best;
}

// -----------------------------------------------------------------------------------------
// GraphicsWindowGBM
// -----------------------------------------------------------------------------------------

class GraphicsWindowGBM: public osgViewer::GraphicsWindow {
public:
	explicit GraphicsWindowGBM(osg::GraphicsContext::Traits* traits) {
		_traits = traits;

		init();
	}

	~GraphicsWindowGBM() override {
		close(true);
	}

	bool valid() const override { return _valid; }

	void init() {
		if(_initialized) return;

		if(!_traits) {
			osg::notify(osg::FATAL) << "GBM: no traits" << std::endl;

			return;
		}

		_traits->windowDecoration = false;
		_traits->pbuffer = false;

		if(!initDrm()) return;
		if(!initGbm()) return;
		if(!initEgl()) return;

		osg::ref_ptr<osg::State> state = new osg::State();

		state->setGraphicsContext(this);
		state->setContextID(osg::GraphicsContext::createNewContextID());

		setState(state);

		_initialized = true;
		_realized = true;
		_valid = true;

		osg::notify(osg::NOTICE)
			<< "GBM/EGL initialized: mode=" << _mode.hdisplay << "x" << _mode.vdisplay
			<< " EGL=" << _eglMajor << "." << _eglMinor
			<< std::endl
		;
	}

	bool realizeImplementation() override {
		if(!_initialized) init();

		_realized = _valid;

		return _realized;
	}

	bool isRealizedImplementation() const override { return _realized; }

	bool makeCurrentImplementation() override {
		if(eglMakeCurrent(_eglDisplay, _eglSurface, _eglSurface, _eglContext) == EGL_TRUE) return true;

		osg::notify(osg::FATAL) << "GBM: eglMakeCurrent failed" << std::endl;

		return false;
	}

	bool releaseContextImplementation() override {
		return eglMakeCurrent(_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
	}

	void swapBuffersImplementation() override {
		if(!_valid) return;

		if(eglSwapBuffers(_eglDisplay, _eglSurface) != EGL_TRUE) {
			osg::notify(osg::FATAL) << "GBM: eglSwapBuffers failed" << std::endl;

			return;
		}

		gbm_bo* nextBo = gbm_surface_lock_front_buffer(_gbmSurface);

		if(!nextBo) {
			osg::notify(osg::FATAL) << "GBM: gbm_surface_lock_front_buffer failed" << std::endl;

			return;
		}

		DrmFb* fb = drmFbGetFromBo(nextBo);

		if(!fb) {
			gbm_surface_release_buffer(_gbmSurface, nextBo);

			return;
		}

		if(!_modeSet) {
			if(drmModeSetCrtc(_drmFd, _crtcId, fb->fbId, 0, 0, &_connectorId, 1, &_mode) != 0) {
				osg::notify(osg::FATAL)
					<< "GBM: drmModeSetCrtc failed: " << std::strerror(errno) << std::endl;

				gbm_surface_release_buffer(_gbmSurface, nextBo);

				return;
			}

			_modeSet = true;
		}

		else {
			_waitingForFlip = true;

			if(drmModePageFlip(_drmFd, _crtcId, fb->fbId, DRM_MODE_PAGE_FLIP_EVENT, this) != 0) {
				osg::notify(osg::FATAL)
					<< "GBM: drmModePageFlip failed: " << std::strerror(errno) << std::endl;

				gbm_surface_release_buffer(_gbmSurface, nextBo);

				_waitingForFlip = false;

				return;
			}

			waitForPageFlip();
		}

		if(_currentBo) gbm_surface_release_buffer(_gbmSurface, _currentBo);

		_currentBo = nextBo;
	}

	void closeImplementation() override {
		restoreCrtc();

		// Must release any outstanding GBM buffer BEFORE eglTerminate(): at least the Nvidia
		// EGL/GBM platform driver (libnvidia-egl-gbm.so) ties buffer-object release back to the
		// still-alive EGL/GBM association, so releasing it after EGL is torn down dereferences
		// already-freed driver state and segfaults.
		if(_currentBo && _gbmSurface) {
			gbm_surface_release_buffer(_gbmSurface, _currentBo);

			_currentBo = nullptr;
		}

		if(_eglDisplay != EGL_NO_DISPLAY) {
			// Released only if it is this window's own context that is current: releasing with
			// EGL_NO_CONTEXT drops the calling thread's current context even when it belongs to
			// another display.
			if(_eglContext != EGL_NO_CONTEXT && eglGetCurrentContext() == _eglContext) {
				eglMakeCurrent(_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			}

			if(_eglContext != EGL_NO_CONTEXT) eglDestroyContext(_eglDisplay, _eglContext);
			if(_eglSurface != EGL_NO_SURFACE) eglDestroySurface(_eglDisplay, _eglSurface);

			eglTerminate(_eglDisplay);
		}

		if(_gbmSurface) gbm_surface_destroy(_gbmSurface);
		if(_gbmDevice) gbm_device_destroy(_gbmDevice);

		if(_drmConnector) drmModeFreeConnector(_drmConnector);
		if(_drmResources) drmModeFreeResources(_drmResources);
		if(_origCrtc) drmModeFreeCrtc(_origCrtc);

		if(_drmFd >= 0) ::close(_drmFd);

		_eglDisplay = EGL_NO_DISPLAY;
		_eglContext = EGL_NO_CONTEXT;
		_eglSurface = EGL_NO_SURFACE;
		_eglConfig = nullptr;

		_gbmSurface = nullptr;
		_gbmDevice = nullptr;

		_drmConnector = nullptr;
		_drmResources = nullptr;
		_origCrtc = nullptr;
		_drmFd = -1;

		_initialized = false;
		_realized = false;
		_valid = false;
		_modeSet = false;
	}

private:
	bool initDrm() {
		_drmFd = openDrmCard();

		if(_drmFd < 0) return false;

		_drmResources = drmModeGetResources(_drmFd);

		if(!_drmResources) {
			osg::notify(osg::FATAL) << "DRM: drmModeGetResources failed" << std::endl;

			return false;
		}

		for(int i = 0; i < _drmResources->count_connectors; i++) {
			drmModeConnector* conn = drmModeGetConnector(_drmFd, _drmResources->connectors[i]);

			if(!conn) continue;

			if(conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
				_drmConnector = conn;

				break;
			}

			drmModeFreeConnector(conn);
		}

		if(!_drmConnector) {
			osg::notify(osg::FATAL) << "DRM: no connected connector found" << std::endl;

			return false;
		}

		_connectorId = _drmConnector->connector_id;
		_mode = chooseDrmMode(
			_drmConnector,
			static_cast<unsigned int>(_traits->width),
			static_cast<unsigned int>(_traits->height)
		);
		_traits->width = _mode.hdisplay;
		_traits->height = _mode.vdisplay;

		_crtcId = findCrtcForConnector(_drmFd, _drmResources, _drmConnector);

		if(!_crtcId) {
			osg::notify(osg::FATAL)
				<< "DRM: no usable CRTC found for connector " << _connectorId << std::endl;

			return false;
		}

		_origCrtc = drmModeGetCrtc(_drmFd, _crtcId);

		if(!_origCrtc) osg::notify(osg::WARN)
			<< "DRM: could not save original CRTC state (restore on exit will be skipped)"
			<< std::endl;

		osg::notify(osg::NOTICE)
			<< "DRM: connector=" << _connectorId
			<< " crtc=" << _crtcId
			<< " mode=" << _mode.hdisplay << "x" << _mode.vdisplay << "@" << _mode.vrefresh << "Hz"
			<< std::endl
		;

		return true;
	}

	bool initGbm() {
		_gbmDevice = gbm_create_device(_drmFd);

		if(!_gbmDevice) {
			osg::notify(osg::FATAL) << "GBM: gbm_create_device failed" << std::endl;

			return false;
		}

		_gbmSurface = gbm_surface_create(
			_gbmDevice,
			static_cast<uint32_t>(_traits->width),
			static_cast<uint32_t>(_traits->height),
			GBM_FORMAT_XRGB8888,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING
		);

		if(!_gbmSurface) {
			osg::notify(osg::FATAL) << "GBM: gbm_surface_create failed" << std::endl;

			return false;
		}

		return true;
	}

	bool initEgl() {
		auto eglGetPlatformDisplayEXT = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
			eglGetProcAddress("eglGetPlatformDisplayEXT")
		);

		_eglDisplay = eglGetPlatformDisplayEXT ?
			eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, _gbmDevice, nullptr) :
			eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(_gbmDevice));

		if(_eglDisplay == EGL_NO_DISPLAY) {
			osg::notify(osg::FATAL) << "GBM: eglGetDisplay failed" << std::endl;

			return false;
		}

		if(!eglInitialize(_eglDisplay, &_eglMajor, &_eglMinor)) {
			osg::notify(osg::FATAL) << "GBM: eglInitialize failed" << std::endl;

			return false;
		}

		if(!eglBindAPI(EGL_OPENGL_API)) {
			osg::notify(osg::FATAL) << "GBM: eglBindAPI(EGL_OPENGL_API) failed" << std::endl;

			return false;
		}

		const EGLint configAttribs[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 0,
			EGL_DEPTH_SIZE, 24,
			EGL_NONE
		};

		EGLint numConfigs = 0;

		if(!eglChooseConfig(_eglDisplay, configAttribs, &_eglConfig, 1, &numConfigs) || numConfigs < 1) {
			osg::notify(osg::FATAL) << "GBM: eglChooseConfig failed" << std::endl;

			return false;
		}

		_eglContext = eglCreateContext(_eglDisplay, _eglConfig, EGL_NO_CONTEXT, nullptr);

		if(_eglContext == EGL_NO_CONTEXT) {
			osg::notify(osg::FATAL) << "GBM: eglCreateContext failed" << std::endl;

			return false;
		}

		_eglSurface = eglCreateWindowSurface(
			_eglDisplay,
			_eglConfig,
			reinterpret_cast<EGLNativeWindowType>(_gbmSurface),
			nullptr
		);

		if(_eglSurface == EGL_NO_SURFACE) {
			osg::notify(osg::FATAL) << "GBM: eglCreateWindowSurface failed" << std::endl;

			return false;
		}

		// Verifies the context is usable, then releases it: a context is only current between the
		// viewer's makeCurrent()/releaseContext() calls. Left current, it stays bound to this
		// thread until the first frame (same as GraphicsWindowEGL).
		if(!eglMakeCurrent(_eglDisplay, _eglSurface, _eglSurface, _eglContext)) {
			osg::notify(osg::FATAL) << "GBM: initial eglMakeCurrent failed" << std::endl;

			return false;
		}

		eglMakeCurrent(_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

		return true;
	}

	static void pageFlipHandler(int, unsigned int, unsigned int, unsigned int, void* data) {
		auto* self = static_cast<GraphicsWindowGBM*>(data);

		if(self) self->_waitingForFlip = false;
	}

	void waitForPageFlip() {
		drmEventContext ev = {};

		ev.version = 2;
		ev.page_flip_handler = &GraphicsWindowGBM::pageFlipHandler;

		while(_waitingForFlip) {
			pollfd pfd = {};

			pfd.fd = _drmFd;
			pfd.events = POLLIN;

			int ret = poll(&pfd, 1, 3000);

			if(ret < 0) {
				if(errno == EINTR) continue;

				osg::notify(osg::FATAL) << "DRM: poll failed: " << std::strerror(errno) << std::endl;

				_waitingForFlip = false;

				break;
			}

			if(ret == 0) {
				osg::notify(osg::FATAL) << "DRM: page flip timed out" << std::endl;

				_waitingForFlip = false;

				break;
			}

			if(pfd.revents & (POLLERR | POLLHUP)) {
				osg::notify(osg::FATAL) << "DRM: poll error on DRM fd" << std::endl;

				_waitingForFlip = false;

				break;
			}

			if(pfd.revents & POLLIN) drmHandleEvent(_drmFd, &ev);
		}
	}

	void restoreCrtc() {
		if(_origCrtc && _drmFd >= 0) drmModeSetCrtc(
			_drmFd,
			_origCrtc->crtc_id,
			_origCrtc->buffer_id,
			_origCrtc->x,
			_origCrtc->y,
			&_connectorId,
			1,
			&_origCrtc->mode
		);
	}

private:
	bool _valid = false;
	bool _initialized = false;
	bool _realized = false;
	bool _modeSet = false;
	bool _waitingForFlip = false;

	int _drmFd = -1;

	drmModeRes* _drmResources = nullptr;
	drmModeConnector* _drmConnector = nullptr;
	drmModeCrtc* _origCrtc = nullptr;

	uint32_t _connectorId = 0;
	uint32_t _crtcId = 0;
	drmModeModeInfo _mode = {};

	gbm_device* _gbmDevice = nullptr;
	gbm_surface* _gbmSurface = nullptr;
	gbm_bo* _currentBo = nullptr;

	EGLDisplay _eglDisplay = EGL_NO_DISPLAY;
	EGLContext _eglContext = EGL_NO_CONTEXT;
	EGLSurface _eglSurface = EGL_NO_SURFACE;
	EGLConfig _eglConfig = nullptr;

	EGLint _eglMajor = 0;
	EGLint _eglMinor = 0;
};

}

osg::ref_ptr<osgViewer::GraphicsWindow> createGBMWindow(osg::GraphicsContext::Traits* traits) {
	return new GraphicsWindowGBM(traits);
}

}
