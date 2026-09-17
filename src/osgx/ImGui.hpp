#pragma once

#include "Debug.hpp"

#ifdef OSGX_IMGUI

OSGX_DISABLE_WARNINGS

#include <osg/Texture2D>
#include <osg/Texture2DArray>
#include <osg/Stats>

OSGX_ENABLE_WARNINGS

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <cstring>
#include <tuple>
#include <unordered_map>
#endif

namespace osgx {

#ifdef OSGX_IMGUI
namespace imgui {

// Vendored ext/imgui is pinned to a plain release tag, not the docking branch (no
// IMGUI_HAS_DOCK/DockSpaceOverViewport), so there's no drag-to-dock here the way
// osgEarth's ImGuiEventHandler does it with its own vendored docking-branch imgui.h.
// Dock::LEFT/RIGHT instead just pins the "osgx::imgui" window to that edge, full
// viewport height, every frame - no dragging, but no extra build surface either.
enum class Dock { NONE, LEFT, RIGHT };

// Controls what the Widget window shows.
struct Options {
	bool showGPUInfo = true;
	bool showFrameInfo = true;
	Dock dock = Dock::NONE;
	float dockWidth = 340.0f;
};

// Per-section knobs for Widget::addSection() - a growable bag of named options
// rather than more positional bools, since this is expected to grow (a size
// hint beyond expand/constrain, tooltips, etc.). Public/namespace-scope on
// purpose: this is what a caller constructs directly (Python included), unlike
// Widget's own private Section storage struct, which just holds one of these.
struct SectionOptions {
	// true: divide up whatever vertical space is left over after all
	// non-expanding sections have drawn at their natural height (see
	// Widget::render()). Meant for the rare section that actually wants to
	// grow (Profiler's scrolling table), not a general layout system.
	bool expand = false;

	// true: CollapsingHeader starts open. Sections start collapsed unless the
	// caller explicitly requests otherwise.
	bool defaultOpen = false;

	static SectionOptions create(bool expand=false, bool defaultOpen=false) {
		return {.expand = expand, .defaultOpen = defaultOpen};
	}
};

// Small value-oriented Dear ImGui adapters. They are deliberately useful from
// both C++ and Python: C++ callers get a (changed, value) result instead of an
// out-parameter, while Python can expose the exact same behavior as a tuple.
std::pair<bool, float> sliderFloat(
	const std::string& label,
	float value,
	float min,
	float max,
	const char* format="%.3f"
);

std::pair<bool, float> sliderFloatNudge(
	const std::string& label,
	float value,
	float min,
	float max,
	float stepPct=0.01f,
	const char* format="%.3f"
);

inline void text(const std::string& value) {
	ImGui::TextUnformatted(value.c_str());
}

inline void separator() {
	ImGui::Separator();
}

inline std::pair<bool, bool> checkbox(const std::string& label, bool value) {
	return {ImGui::Checkbox(label.c_str(), &value), value};
}

inline bool button(const std::string& label) {
	return ImGui::Button(label.c_str());
}

std::tuple<bool, float, float, float> colorEdit3(
	const std::string& label,
	float r,
	float g,
	float b
);

std::pair<bool, std::string> inputText(
	const std::string& label,
	std::string value,
	int maxLength=256,
	bool enterReturnsTrue=false
);

std::pair<bool, int> radioGroup(
	int value,
	const std::vector<std::string>& labels,
	bool sameLine=true
);

// Collapsed alternative to radioGroup() - one line showing the current selection,
// expands into the label list on click instead of laying every option out at once.
std::pair<bool, int> combo(
	const std::string& label,
	int value,
	const std::vector<std::string>& labels
);

void drawTexture2D(
	osg::Texture2D* texture,
	osg::RenderInfo& ri,
	unsigned int width=0,
	unsigned int height=0
);

// Texture2DArray cannot be sampled by ImGui's sampler2D shader, so we display
// metadata only: dimensions, layer count, and GL object ID.
void drawTexture2DArray(
	osg::Texture2DArray* texture,
	osg::RenderInfo& ri,
	unsigned int thumbSize=0
);

// Scene texture browser. Discovers Texture2D and Texture2DArray attributes on
// node StateSets. Texture2D entries are shown as thumbnails; Texture2DArray
// entries show metadata only (GL_TEXTURE_2D_ARRAY is incompatible with ImGui's
// sampler2D shader). All display goes through live GL texture objects.
class TextureSection {
public:
	// Takes osgViewer::View (not the full Viewer) - this is the only Section that
	// genuinely needs a live reference held across frames, because getSceneData()
	// must reflect whatever the scene root CURRENTLY is (it can change after this
	// is constructed - see the async-load pattern where the scene starts empty
	// and a real model attaches seconds later). Everything else this class could
	// possibly want is either one-time (nothing here is) or cacheable, so View is
	// the narrowest type that still lets refresh() ask that question live.
	TextureSection(osgViewer::View& view, osg::Node* root = nullptr):
	_view(&view),
	_root(root) {}

	void operator()(osg::RenderInfo& ri);
	void refresh(osg::RenderInfo& ri);

private:
	osg::observer_ptr<osgViewer::View> _view;
	osg::observer_ptr<osg::Node> _root;
	std::vector<osg::ref_ptr<osg::Texture>> _textures;

	float _thumbnailSize = 64.0f;
	bool _scanned = false;
};

// ImGui front-end for OSG's aggregate osg::Stats counters. This mirrors the
// useful parts of osgViewer::StatsHandler without installing its HUD camera.
class StatsSection {
public:
	// osgViewer::Viewer& is only a CONSTRUCTOR parameter, never stored - both
	// getViewerStats() and getCameras() are ViewerBase-only (not on osg::View), but
	// both are one-time setup here, and the osg::Stats*/master osg::Camera* they
	// return are stable for the Viewer's whole lifetime, so they're captured once
	// below instead of re-asked from a held Viewer reference every frame.
	explicit StatsSection(osgViewer::Viewer& viewer);

	void operator()(osg::RenderInfo& ri) const;

private:
	osg::ref_ptr<osg::Stats> _viewerStats;
	osg::observer_ptr<osg::Camera> _masterCamera;

	static constexpr ImGuiTableFlags _tableFlags =
		ImGuiTableFlags_Borders |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_SizingStretchProp
	;

	static bool _latest(osg::Stats* stats, const std::string& name, double& value);
	static void _valueText(bool valid, double value, double multiplier, const char* fmt);

	static void _row(
		osg::Stats* stats,
		const std::string& name,
		const char* label,
		double multiplier,
		bool averageInInverseSpace,
		const char* fmt
	);

	static void _latestRow(
		osg::Stats* stats,
		const std::string& name,
		const char* label,
		const char* fmt
	);
};

// Callable that installs its own profiler backend on construction and draws
// the GPU timing table + SYNC/ASYNC toggle when invoked.
// Pass to Widget::addSection() or use Widget::addProfilerSection() shortcut.
class ProfilerSection {
public:
	// Only ever touches view.getCamera(), transiently, right here - nothing to
	// hold onto afterward, so this never stored a Viewer/View reference at all.
	// printEvery forwards to ProfilerFinalCallback (0 = never print to the log;
	// SYNC/ASYNC is still toggleable live via the section's own radio buttons,
	// so it isn't a constructor parameter here).
	ProfilerSection(osgViewer::View& view, osg::Node* sceneRoot, size_t printEvery=0);

	// Re-scan a dynamic scene graph. Existing profiler callbacks are updated in
	// place, while newly paged/attached drawables receive callbacks for the first
	// time. Useful for terrain engines and streaming scene loaders.
	void refresh();

	void operator()(osg::RenderInfo& ri);

private:
	osg::observer_ptr<osg::Node> _sceneRoot;
	debug::ProfilerFinalCallback<>* _finalCb = nullptr;
};

// Reusable osgx::imgui content for an application-owned Dear ImGui frame. Panel
// deliberately creates no ImGui context, backend, window, or event handler:
// call draw() after the host has begun the window that should contain these
// sections. Widget below is the convenience driver that supplies those pieces.
class Panel {
public:
	void addSection(std::string label, std::function<void(osg::RenderInfo&)> fn, SectionOptions options={}) {
		_sections.push_back({std::move(label), std::move(fn), options});
	}

	void removeSection(const std::string& label) {
		std::erase_if(_sections, [&](const auto& section) { return section.label == label; });
	}

	void clearSections() {
		_sections.clear();
	}

	// Forces a section's CollapsingHeader open/closed every frame (ImGui::SetNextItemOpen,
	// ImGuiCond_Always), overriding both the user's own manual toggling and the one-time
	// SectionOptions::defaultOpen seed - SectionOptions::defaultOpen only ever applies the
	// FIRST time ImGui sees a given label's ID, so it can't express "reopen this section
	// whenever the app selects it again" on its own. Meant for exactly that app-driven case
	// (e.g. a gallery that expands the section for whatever item was just clicked); not a
	// general "set it once" API. A label with no override here falls back to normal
	// user-toggleable behavior via defaultOpen, same as before this existed.
	void setSectionOpen(const std::string& label, bool open) {
		_forcedOpen[label] = open;
	}

	void clearSectionOpen(const std::string& label) {
		_forcedOpen.erase(label);
	}

	void addProfilerSection(osgViewer::View& view, osg::Node* sceneRoot, bool defaultOpen=false, size_t printEvery=0) {
		addSection("Profiler", ProfilerSection(view, sceneRoot, printEvery), {
			.expand = true, .defaultOpen = defaultOpen
		});
	}

	void addStatsSection(osgViewer::Viewer& viewer, bool defaultOpen=false) {
		addSection("Stats", StatsSection(viewer), {.defaultOpen = defaultOpen});
	}

	void addTextureSection(osgViewer::View& view, osg::Node* root, bool defaultOpen=false) {
		addSection("Textures", TextureSection(view, root), {.defaultOpen = defaultOpen});
	}

	void addTextureSection(osgViewer::View& view, bool defaultOpen=false) {
		addSection("Textures", TextureSection(view), {.defaultOpen = defaultOpen});
	}

	void draw(osg::RenderInfo& ri);

private:
	struct Section {
		std::string label;
		std::function<void(osg::RenderInfo&)> fn;
		SectionOptions options;
	};

	std::vector<Section> _sections;
	std::unordered_map<std::string, bool> _forcedOpen;
};

// Owns the ImGui lifecycle for one viewer window.
// Requires the viewer already be osgViewer::Viewer::SingleThreaded (Dear ImGui's
// single global ImGuiContext/IO state isn't safe to touch from more than one OSG
// draw thread) - same requirement as osgEarth's own ImGuiEventHandler, which
// likewise leaves it entirely to the caller (see its applications/osgearth_imgui
// example: setThreadingModel is called before the handler is ever constructed).
// We used to check-and-force this ourselves, but that required holding a
// ViewerBase-shaped reference for a one-time check nothing else here needs --
// narrowing to osgViewer::View below means this class no longer even has access
// to the threading model at all, which is the point: it's the caller's job now.
// Pushes itself to the front of the view's event handler list. Sections are
// drawn in order inside one window.
class Widget: public osgGA::GUIEventHandler {
public:
	// drawCamera lets an app pin the ImGui NewFrame/Render pair to a specific
	// camera instead of the default master-camera/slave-0 guess (see handle()
	// below). Needed by deferred-rendering apps with their own downstream
	// POST_RENDER compositing camera nested under the scene graph (not a View
	// slave) - that camera draws AFTER the master camera's own
	// PostDrawCallback, so ImGui's draw would render then be immediately
	// overwritten by the later fullscreen composite, while its mouse-capture
	// bookkeeping (computed in NewFrame(), independent of what's actually
	// still visible on screen) stays live - an invisible rectangle that eats
	// mouse input. Pass that camera explicitly to fix it.
	Widget(osgViewer::View& view, osg::Camera* drawCamera=nullptr, Options opts={}):
	_masterCamera(view.getCamera()),
	_opts(opts),
	_drawCamera(drawCamera) {
		view.getEventHandlers().push_front(this);
	}

	Panel& panel() { return _panel; }
	const Panel& panel() const { return _panel; }
	void addSection(std::string label, std::function<void(osg::RenderInfo&)> fn, SectionOptions options={}) { _panel.addSection(std::move(label), std::move(fn), options); }
	void removeSection(const std::string& label) { _panel.removeSection(label); }
	void clearSections() { _panel.clearSections(); }
	void setSectionOpen(const std::string& label, bool open) { _panel.setSectionOpen(label, open); }
	void clearSectionOpen(const std::string& label) { _panel.clearSectionOpen(label); }
	void addProfilerSection(osgViewer::View& view, osg::Node* root, bool defaultOpen=false, size_t printEvery=0) { _panel.addProfilerSection(view, root, defaultOpen, printEvery); }
	void addStatsSection(osgViewer::Viewer& viewer, bool defaultOpen=false) { _panel.addStatsSection(viewer, defaultOpen); }
	void addTextureSection(osgViewer::View& view, osg::Node* root, bool defaultOpen=false) { _panel.addTextureSection(view, root, defaultOpen); }
	void addTextureSection(osgViewer::View& view, bool defaultOpen=false) { _panel.addTextureSection(view, defaultOpen); }

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;

private:
	osg::observer_ptr<osg::Camera> _masterCamera;
	Options _opts;
	osg::observer_ptr<osg::Camera> _drawCamera;
	bool _initialized = false;
	bool _firstFrame = true;
	double _time = 0.0;
	Panel _panel;

	void newFrame(osg::RenderInfo& ri);
	void render(osg::RenderInfo& ri);

	struct PreDraw: public osg::Camera::DrawCallback {
		Widget& _w;

		explicit PreDraw(Widget& w): _w(w) {}

		void operator()(osg::RenderInfo& ri) const override { _w.newFrame(ri); }
	};

	struct PostDraw: public osg::Camera::DrawCallback {
		Widget& _w;

		explicit PostDraw(Widget& w): _w(w) {}

		void operator()(osg::RenderInfo& ri) const override { _w.render(ri); }
	};
};

}
#endif // OSGX_IMGUI

}
