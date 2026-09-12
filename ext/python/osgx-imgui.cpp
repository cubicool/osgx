#include "osgx-python.hpp"
#include "osgx/ImGui.hpp"

// osgx::imgui - deliberately NOT a general ImGui wrapper (that's pyimgui's job elsewhere). Just
// enough to build quick debugging knobs inside a Widget::addSection() callback: a handful of
// stateless functions returning (changed, value) tuples, since Python floats/bools aren't
// mutable references the way ImGui's C++ &value out-params expect. A sibling of osgx::debug, not
// nested under it - most of this (Panel/Widget/sliders) has nothing to do with the profiler;
// only ProfilerSection reaches into debug::.
//
// This whole file only compiles (and is only added to the source list, see CMakeLists.txt) when
// OSGX_WITH_IMGUI is on - osgx::imgui's own types don't exist otherwise (see osgx/ImGui.hpp).
#ifdef OSGX_IMGUI

namespace pybind11x {
	template<>
	void kwargs_init_own(osgx::imgui::Options& self, const py::kwargs& kwargs) {
		if(kwargs.contains("show_gpu_info")) self.showGPUInfo = kwargs["show_gpu_info"].cast<bool>();

		if(kwargs.contains("show_frame_info")) self.showFrameInfo = kwargs["show_frame_info"].cast<bool>();

		if(kwargs.contains("dock")) self.dock = kwargs["dock"].cast<osgx::imgui::Dock>();

		if(kwargs.contains("dock_width")) self.dockWidth = kwargs["dock_width"].cast<float>();
	}
}

namespace osgx_python {

void bind_imgui(py::module_& m_imgui) {
	py::enum_<osgx::imgui::Dock>(
		m_imgui,
		"Dock",
		"Where the osgx::imgui window pins itself. LEFT/RIGHT pin it to that edge at full "
		"viewport height every frame (vendored imgui is a plain release tag, not the docking "
		"branch, so there's no drag-to-dock)."
	)
		.value("NONE", osgx::imgui::Dock::NONE)
		.value("LEFT", osgx::imgui::Dock::LEFT)
		.value("RIGHT", osgx::imgui::Dock::RIGHT)
		.export_values()
	;

	py::class_<osgx::imgui::Options>(
		m_imgui,
		"Options",
		"Controls what a Widget window shows: GPU/frame info headers and docking."
	)
		// Use the same kwargs_init convention as OpenSceneGraph.py's OSG wrappers, so
		// the construction spelling stays extensible as this small options bag grows.
		.def(py::init([](py::kwargs kwargs) {
			osgx::imgui::Options result;

			pyx::kwargs_init(result, kwargs);

			return result;
		}), "Constructs Options from any subset of show_gpu_info/show_frame_info/dock/dock_width keywords.")
		.def_readwrite("show_gpu_info", &osgx::imgui::Options::showGPUInfo, "Whether the window shows a GPU info header.")
		.def_readwrite("show_frame_info", &osgx::imgui::Options::showFrameInfo, "Whether the window shows a frame info header.")
		.def_readwrite("dock", &osgx::imgui::Options::dock, "Which edge (if any) the window pins itself to.")
		.def_readwrite("dock_width", &osgx::imgui::Options::dockWidth, "Window width in pixels when docked LEFT/RIGHT.")
	;

	// A growable options bag for addSection() - expected to grow (a size hint
	// beyond expand/constrain, tooltips, etc.), so this is keyword-constructible
	// from Python rather than adding more positional args to addSection itself.
	py::class_<osgx::imgui::SectionOptions>(
		m_imgui,
		"SectionOptions",
		"Per-section knobs for Panel.addSection()/Widget.addSection()."
	)
		.def(
			py::init(&osgx::imgui::SectionOptions::create),
			"expand"_a=false,
			"default_open"_a=false,
			"Constructs section options. `expand`=True divides up leftover vertical space among "
			"expanding sections (meant for the rare section that should grow, like a scrolling "
			"profiler table). `default_open`=True starts the section's CollapsingHeader open."
		)
		.def_readwrite(
			"expand", &osgx::imgui::SectionOptions::expand,
			"True: this section shares leftover vertical space after non-expanding sections draw at natural height."
		)
		.def_readwrite(
			"default_open", &osgx::imgui::SectionOptions::defaultOpen,
			"True: this section's CollapsingHeader starts open instead of collapsed."
		)
	;

	py::class_<osgx::imgui::Panel>(
		m_imgui,
		"Panel",
		"Reusable osgx.imgui content for an application-owned Dear ImGui frame. Creates no ImGui "
		"context, backend, window, or event handler of its own - call draw() after the host has "
		"begun the window that should contain these sections. See Widget for the convenience "
		"driver that supplies those pieces automatically."
	)
		.def(py::init<>(), "Constructs an empty Panel with no sections.")
		.def(
			"addSection", &osgx::imgui::Panel::addSection,
			"label"_a,
			"fn"_a,
			"options"_a=osgx::imgui::SectionOptions(),
			"Adds a named section: `fn(render_info)` is called to draw its contents inside a "
			"CollapsingHeader each frame draw() runs."
		)
		.def("removeSection", &osgx::imgui::Panel::removeSection, "label"_a, "Removes the section named `label`, if present.")
		.def("clearSections", &osgx::imgui::Panel::clearSections, "Removes every section.")
		.def(
			"setSectionOpen", &osgx::imgui::Panel::setSectionOpen, "label"_a, "open"_a,
			"Forces section `label`'s CollapsingHeader open/closed every frame, overriding both "
			"user toggling and the section's own default_open seed. For app-driven cases (e.g. a "
			"gallery expanding whatever item was just clicked), not a general set-once API."
		)
		.def(
			"clearSectionOpen", &osgx::imgui::Panel::clearSectionOpen, "label"_a,
			"Removes a forced-open override set via setSectionOpen(), returning `label` to normal "
			"user-toggleable behavior."
		)
		.def(
			"addStatsSection",
			&osgx::imgui::Panel::addStatsSection,
			"viewer"_a,
			"default_open"_a=false,
			"Adds a built-in \"Stats\" section showing osgViewer::Viewer stats."
		)
		.def(
			"addProfilerSection",
			&osgx::imgui::Panel::addProfilerSection,
			"view"_a,
			"sceneRoot"_a,
			"default_open"_a=false,
			"print_every"_a=0,
			"Adds a built-in \"Profiler\" section showing osgx.debug per-drawable GPU/CPU timing "
			"for `sceneRoot`, with progress-bar percentages and a SYNC/ASYNC toggle."
		)
		.def(
			"addTextureSection",
			py::overload_cast<
				osgViewer::View&,
				osg::Node*, bool
			>(&osgx::imgui::Panel::addTextureSection),
			"view"_a,
			"root"_a,
			"default_open"_a=false,
			"Adds a built-in \"Textures\" section listing every osg.Texture found under `root`."
		)
		.def(
			"addTextureSection",
			py::overload_cast<osgViewer::View&, bool>(&osgx::imgui::Panel::addTextureSection),
			"view"_a,
			"default_open"_a=false,
			"Adds a built-in \"Textures\" section listing every osg.Texture found under `view`'s scene data."
		)
		.def(
			"draw", &osgx::imgui::Panel::draw, "render_info"_a,
			"Draws every section's contents inside the host's already-begun ImGui window."
		)
	;

	py::class_<
		osgx::imgui::Widget,
		osgGA::GUIEventHandler,
		osg::ref_ptr<osgx::imgui::Widget>
	>(
		m_imgui,
		"Widget",
		"Owns the Dear ImGui lifecycle (context, backend, window, event handling) for one viewer "
		"window and draws its Panel's sections inside it every frame. Requires the viewer already "
		"be osgViewer.Viewer.SingleThreaded - Dear ImGui's single global context isn't safe to "
		"touch from more than one OSG draw thread. Add via addEventHandler()."
	)
		.def(
			// osgViewer::View, not the full Viewer - Widget only needs getCamera()
			// (cached once) and getEventHandlers(); a Python osgViewer::Viewer still
			// works here since it upcasts (View is separately registered above and
			// Viewer's py::class_ lists it as a base).
			py::init<osgViewer::View&, osg::Camera*, osgx::imgui::Options>(),
			"viewer"_a,
			"draw_camera"_a=nullptr,
			"options"_a=osgx::imgui::Options(),
			"Constructs a Widget for `viewer`. `draw_camera` defaults to the viewer's own camera; "
			"pass a different one for a multi-camera setup where ImGui should draw on a specific pass."
		)
		.def(
			"addSection",
			&osgx::imgui::Widget::addSection,
			"label"_a,
			"fn"_a,
			"options"_a=osgx::imgui::SectionOptions(),
			"Adds a named section to this Widget's own Panel; same contract as Panel.addSection()."
		)
		.def("removeSection", &osgx::imgui::Widget::removeSection, "label"_a, "Removes the section named `label`, if present.")
		.def("clearSections", &osgx::imgui::Widget::clearSections, "Removes every section.")
		.def(
			"setSectionOpen", &osgx::imgui::Widget::setSectionOpen, "label"_a, "open"_a,
			"Forces section `label`'s CollapsingHeader open/closed every frame; same contract as Panel.setSectionOpen()."
		)
		.def(
			"clearSectionOpen", &osgx::imgui::Widget::clearSectionOpen, "label"_a,
			"Removes a forced-open override set via setSectionOpen()."
		)
		// Widget no longer holds a Viewer/View reference of its own (see the C++
		// class comment), so these now take one explicitly instead of reusing
		// whatever Widget was constructed with.
		.def(
			"addStatsSection",
			&osgx::imgui::Widget::addStatsSection,
			"viewer"_a,
			"default_open"_a=false,
			"Adds a built-in \"Stats\" section showing osgViewer::Viewer stats."
		)
		.def(
			"addProfilerSection",
			&osgx::imgui::Widget::addProfilerSection,
			"view"_a,
			"sceneRoot"_a,
			"default_open"_a=false,
			"print_every"_a=0,
			"Adds a built-in \"Profiler\" section showing osgx.debug per-drawable GPU/CPU timing "
			"for `sceneRoot`, with progress-bar percentages and a SYNC/ASYNC toggle."
		)
		.def(
			"addTextureSection",
			py::overload_cast<osgViewer::View&, osg::Node*, bool>(
				&osgx::imgui::Widget::addTextureSection
			),
			"view"_a,
			"root"_a,
			"default_open"_a=false,
			"Adds a built-in \"Textures\" section listing every osg.Texture found under `root`."
		)
		.def(
			"addTextureSection",
			py::overload_cast<osgViewer::View&, bool>(&osgx::imgui::Widget::addTextureSection),
			"view"_a,
			"default_open"_a=false,
			"Adds a built-in \"Textures\" section listing every osg.Texture found under `view`'s scene data."
		)
	;

	m_imgui
		.def(
			"slider_float",
			&osgx::imgui::sliderFloat,
			"label"_a,
			"value"_a,
			"min"_a,
			"max"_a,
			"format"_a="%.3f",
			"Draws a slider; returns (changed, value) since Python floats aren't mutable "
			"references the way ImGui's C++ &value out-param expects."
		)
		.def(
			"slider_float_nudge",
			&osgx::imgui::sliderFloatNudge,
			"label"_a,
			"value"_a,
			"min"_a,
			"max"_a,
			"step_pct"_a=0.01f,
			"format"_a="%.3f",
			"Draws a slider with small +/- nudge buttons stepping by `step_pct` of the (min, max) "
			"range; returns (changed, value)."
		)
		.def("text", &osgx::imgui::text, "text"_a, "Draws unformatted text.")
		.def("separator", &osgx::imgui::separator, "Draws a horizontal separator line.")
		.def(
			"checkbox",
			&osgx::imgui::checkbox,
			"label"_a,
			"value"_a,
			"Draws a checkbox; returns (changed, value)."
		)
		.def("button", &osgx::imgui::button, "label"_a, "Draws a button; returns True on the frame it was clicked.")
		.def(
			"color_edit3",
			&osgx::imgui::colorEdit3,
			"label"_a,
			"r"_a,
			"g"_a,
			"b"_a,
			"Draws an RGB color editor; returns (changed, r, g, b)."
		)
		.def(
			"input_text",
			&osgx::imgui::inputText,
			"label"_a,
			"value"_a,
			"max_length"_a=256,
			"enter_returns_true"_a=false,
			"Draws a single-line text input; returns (changed, value). If `enter_returns_true` is "
			"True, `changed` is only True when Enter is pressed, not on every keystroke."
		)
		.def(
			"radio_group",
			&osgx::imgui::radioGroup,
			"value"_a,
			"labels"_a,
			"same_line"_a=true,
			"Draws a radio button for each of `labels`; returns (changed, value) where `value` is "
			"the selected index. `same_line`=True lays them out horizontally instead of stacked."
		)
	;
}

}

#endif // OSGX_IMGUI
