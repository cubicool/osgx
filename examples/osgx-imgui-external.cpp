// Application-owned Dear ImGui lifecycle + reusable osgx::imgui::Panel.

#include "osgx/Core.hpp"
#include "osgx/ImGui.hpp"
#include "osgx/Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Group>
#include <osg/Shape>
#include <osg/ShapeDrawable>

#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

namespace {

class AppImGuiHost: public osgGA::GUIEventHandler {
public:
	AppImGuiHost(osgViewer::View& view, osgx::imgui::Panel& panel):
		_masterCamera(view.getCamera()),
		_panel(panel) {
		view.getEventHandlers().push_front(this);
	}

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override {
		if(!_installed) {
			if(auto* view = aa.asView()) {
				view->getCamera()->setPreDrawCallback(new PreDraw(*this));
				view->getCamera()->setPostDrawCallback(new PostDraw(*this));
				_installed = true;
			}

			return false;
		}

		if(_firstFrame || ea.getHandled()) return false;

		auto& io = ImGui::GetIO();

		switch(ea.getEventType()) {
		case osgGA::GUIEventAdapter::PUSH:
			io.AddMousePosEvent(ea.getX(), io.DisplaySize.y - ea.getY());
			if(ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			else if(ea.getButton() == osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON) io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
			return io.WantCaptureMouse;

		case osgGA::GUIEventAdapter::RELEASE:
			io.AddMousePosEvent(ea.getX(), io.DisplaySize.y - ea.getY());
			if(ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			else if(ea.getButton() == osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON) io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			return io.WantCaptureMouse;

		case osgGA::GUIEventAdapter::DRAG:
		case osgGA::GUIEventAdapter::MOVE:
			io.AddMousePosEvent(ea.getX(), io.DisplaySize.y - ea.getY());
			return io.WantCaptureMouse;

		case osgGA::GUIEventAdapter::SCROLL:
			io.AddMouseWheelEvent(0.0f, ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_UP ? 1.0f : -1.0f);
			return io.WantCaptureMouse;

		default:
			return false;
		}
	}

private:
	osg::observer_ptr<osg::Camera> _masterCamera;
	osgx::imgui::Panel& _panel;
	bool _installed = false;
	bool _firstFrame = true;
	double _time = 0.0;

	void newFrame(osg::RenderInfo& ri) {
		if(_firstFrame) {
			ImGui::CreateContext();
			ImGui::GetIO().IniFilename = nullptr;
			ImGui_ImplOpenGL3_Init();
			_firstFrame = false;
		}

		ImGui_ImplOpenGL3_NewFrame();
		auto* traits = _masterCamera->getGraphicsContext()->getTraits();
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(traits->width), static_cast<float>(traits->height));

		const double now = ri.getView()->getFrameStamp()->getSimulationTime();
		io.DeltaTime = static_cast<float>(now - _time) + 0.0000001f;
		_time = now;
		ImGui::NewFrame();
	}

	void render(osg::RenderInfo& ri) {
		ImGui::SetNextWindowSize(ImVec2(420.0f, 480.0f), ImGuiCond_FirstUseEver);

		if(ImGui::Begin("Application-owned ImGui")) {
			ImGui::TextDisabled("The host owns context, frame, window, and renderer.");
			ImGui::Separator();
			_panel.draw(ri);
		}

		ImGui::End();
		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	}

	struct PreDraw: public osg::Camera::DrawCallback {
		AppImGuiHost& _host;
		explicit PreDraw(AppImGuiHost& host): _host(host) {}
		void operator()(osg::RenderInfo& ri) const override { _host.newFrame(ri); }
	};

	struct PostDraw: public osg::Camera::DrawCallback {
		AppImGuiHost& _host;
		explicit PostDraw(AppImGuiHost& host): _host(host) {}
		void operator()(osg::RenderInfo& ri) const override { _host.render(ri); }
	};
};

}

int main() {
	auto lib = osgx::initialize();

	osgViewer::Viewer viewer;
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);

	auto root = osgx::make_ref<osg::Group>();
	auto geode = osgx::make_ref<osg::Geode>();
	geode->addDrawable(new osg::ShapeDrawable(new osg::Sphere(osg::Vec3(), 5.0f)));
	root->addChild(geode);
	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());

	osgx::imgui::Panel panel;
	panel.addStatsSection(viewer);
	panel.addProfilerSection(viewer, root);
	panel.addSection("Application Section", [](osg::RenderInfo&) {
		ImGui::Text("This section is supplied by the application.");
	}, {.defaultOpen = true});

	new AppImGuiHost(viewer, panel);

	return viewer.run();
}
