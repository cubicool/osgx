#include "osgx/Cursor.hpp"

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osgViewer/GraphicsWindow>

OSGX_ENABLE_WARNINGS

namespace osgx {

namespace {

osgViewer::GraphicsWindow* graphicsWindow(osgViewer::View& view) {
	auto* camera = view.getCamera();

	if(!camera) return nullptr;

	return dynamic_cast<osgViewer::GraphicsWindow*>(camera->getGraphicsContext());
}

}

void setCursorVisible(osgViewer::View& view, bool visible) {
	auto* gw = graphicsWindow(view);

	if(gw) gw->useCursor(visible);
}

void warpCursor(osgViewer::View& view, float x, float y) {
	view.requestWarpPointer(x, y);
}

bool CursorHandler::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) {
	if(
		_state &&
		(
			ea.getEventType() == osgGA::GUIEventAdapter::MOVE ||
			ea.getEventType() == osgGA::GUIEventAdapter::DRAG
		)
	) {
		_state->updateCursor(static_cast<int>(ea.getX()), static_cast<int>(ea.getY()));
	}

	return false;
}

void CursorCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	if(_state) {
		if(_inWindowCheck) _state->setInWindow(_inWindowCheck());

		if(_fn) _fn(_state->x(), _state->y());
	}

	traverse(node, nv);
}

void CursorCapture::setCaptured(bool captured) {
	if(captured == _captured) return;

	_captured = captured;

	if(auto* view = _view.get()) setCursorVisible(*view, !captured);

	_accum.set(0.0f, 0.0f);
	_recenterPending = captured;
	_echoPending = false;
}

osg::Vec2 CursorCapture::consume() {
	osg::Vec2 result = _accum;

	_accum.set(0.0f, 0.0f);

	return result;
}

bool CursorCapture::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) {
	if(ea.getEventType() == osgGA::GUIEventAdapter::RESIZE) _recenterPending = _captured;

	if(!_captured) return false;

	if(
		ea.getEventType() != osgGA::GUIEventAdapter::MOVE &&
		ea.getEventType() != osgGA::GUIEventAdapter::DRAG
	) return false;

	_center.set(
		0.5f * (ea.getXmin() + ea.getXmax()),
		0.5f * (ea.getYmin() + ea.getYmax())
	);

	if(_recenterPending) {
		_recenterPending = false;
		_echoPending = true;

		aa.requestWarpPointer(_center.x(), _center.y());

		return false;
	}

	// This event may be the echo of our own warp above rather than real user motion: GraphicsWindow
	// implementations typically drain their native event queue in a loop and requestWarpPointer()
	// forces a round trip (e.g. XWarpPointer+XSync on X11) before returning, so the synthetic
	// MOVE/DRAG it generates is often already pending and gets processed in this same pass. That
	// echo rarely lands exactly on _center - float/int truncation in the warp call,
	// plus a Y-flip round trip some GUIActionAdapter implementations apply - so treating it as real
	// input would accumulate a small, consistently-signed leftover delta AND re-warp, forming a
	// self-reinforcing loop that swamps real motion within a single frame. Absorb exactly one event
	// per warp as the presumed echo instead of re-warping again from it.
	if(_echoPending) {
		_echoPending = false;

		return false;
	}

	_accum.x() += ea.getX() - _center.x();
	_accum.y() += ea.getY() - _center.y();

	_echoPending = true;

	aa.requestWarpPointer(_center.x(), _center.y());

	return true;
}

}
