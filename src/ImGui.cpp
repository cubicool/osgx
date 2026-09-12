#include "osgx/ImGui.hpp"

#ifdef OSGX_IMGUI

#include <bit>
#include <cstdint>

namespace osgx::imgui {

namespace {

// ImTextureID's underlying type isn't portable across Dear ImGui versions: older
// (and the default backend-compat) builds define it as `void*`; newer builds
// (system imgui on Ubuntu 26 among them) define it as a plain `ImU64`. A single
// std::bit_cast handles both uniformly - no reinterpret_cast, no if constexpr,
// no template-dependency games - since it doesn't care whether the destination
// is a pointer or an integer, only that the sizes match. Requires
// sizeof(ImTextureID) == sizeof(std::uintptr_t), true whenever ImTextureID is a
// pointer (by definition) and true for ImU64 on any 64-bit target; this project
// doesn't support 32-bit builds, so the static_assert exists purely to turn a
// hypothetical mismatch into a clear error instead of a bit_cast SFINAE wall.
template<typename T>
ImTextureID toTextureID(T id) {
	static_assert(
		sizeof(ImTextureID) == sizeof(std::uintptr_t),
		"ImTextureID must be pointer-sized (no 32-bit support)"
	);

	return std::bit_cast<ImTextureID>(static_cast<std::uintptr_t>(id));
}

}

std::pair<bool, float> sliderFloat(
	const std::string& label,
	float value,
	float min,
	float max,
	const char* format
) {
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);

	return {ImGui::SliderFloat(label.c_str(), &value, min, max, format), value};
}

std::pair<bool, float> sliderFloatNudge(
	const std::string& label,
	float value,
	float min,
	float max,
	float stepPct,
	const char* format
) {
	const float buttonWidth = ImGui::GetFrameHeight();
	const float avail = ImGui::GetContentRegionAvail().x * 0.3f;
	const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
	const float step = (max - min) * stepPct;
	bool changed = false;

	ImGui::PushID(label.c_str());

	if(ImGui::Button("-", ImVec2(buttonWidth, 0.0f))) {
		value = std::clamp(value - step, min, max);
		changed = true;
	}

	ImGui::SameLine(0.0f, spacing);
	ImGui::SetNextItemWidth(std::max(avail - 2.0f * (buttonWidth + spacing), 1.0f));

	if(ImGui::SliderFloat("##slider", &value, min, max, format)) changed = true;

	ImGui::SameLine(0.0f, spacing);

	if(ImGui::Button("+", ImVec2(buttonWidth, 0.0f))) {
		value = std::clamp(value + step, min, max);
		changed = true;
	}

	ImGui::PopID();
	ImGui::SameLine();
	ImGui::TextUnformatted(label.c_str());

	return {changed, value};
}

std::tuple<bool, float, float, float> colorEdit3(
	const std::string& label,
	float r,
	float g,
	float b
) {
	float color[3] = {r, g, b};
	const bool changed = ImGui::ColorEdit3(label.c_str(), color);

	return {changed, color[0], color[1], color[2]};
}

std::pair<bool, std::string> inputText(
	const std::string& label,
	std::string value,
	int maxLength,
	bool enterReturnsTrue
) {
	value.resize(static_cast<std::size_t>(maxLength));
	const ImGuiInputTextFlags flags = enterReturnsTrue
		? ImGuiInputTextFlags_EnterReturnsTrue
		: ImGuiInputTextFlags_None;
	const bool changed = ImGui::InputText(label.c_str(), value.data(), value.size(), flags);

	value.resize(std::strlen(value.c_str()));

	return {changed, std::move(value)};
}

std::pair<bool, int> radioGroup(
	int value,
	const std::vector<std::string>& labels,
	bool sameLine
) {
	bool changed = false;

	for(std::size_t i = 0; i < labels.size(); i++) {
		const int option = static_cast<int>(i);

		if(ImGui::RadioButton(labels[i].c_str(), &value, option)) changed = true;
		if(sameLine && i + 1 < labels.size()) ImGui::SameLine();
	}

	return {changed, value};
}

void drawTexture2D(
	osg::Texture2D* texture,
	osg::RenderInfo& ri,
	unsigned int width,
	unsigned int height
) {
	if(!texture || !ri.getState()) return;

	texture->apply(*ri.getState());

	auto w = static_cast<std::size_t>(std::max(texture->getTextureWidth(), 0));
	auto h = static_cast<std::size_t>(std::max(texture->getTextureHeight(), 0));

	if((!w || !h) && texture->getImage()) {
		w = static_cast<std::size_t>(std::max(texture->getImage()->s(), 0));
		h = static_cast<std::size_t>(std::max(texture->getImage()->t(), 0));
	}

	if(!w || !h) {
		ImGui::TextDisabled("Texture has no size.");

		return;
	}

	const double aspect = static_cast<double>(w) / static_cast<double>(h);

	if(width && height) {
		w = width;
		h = height;
	}

	else if(width) {
		w = width;
		h = static_cast<unsigned int>(static_cast<double>(w) / aspect);
	}

	else if(height) {
		h = height;
		w = static_cast<unsigned int>(static_cast<double>(h) * aspect);
	}

	auto* textureObject = texture->getTextureObject(ri.getState()->getContextID());

	if(!textureObject) {
		ImGui::TextDisabled("Texture object unavailable.");

		return;
	}

	const bool flip = texture->getImage()
		&& texture->getImage()->getOrigin() == osg::Image::TOP_LEFT
	;

	ImGui::Image(
		toTextureID(textureObject->id()),
		ImVec2(static_cast<float>(w), static_cast<float>(h)),
		ImVec2(0.0f, flip ? 0.0f : 1.0f),
		ImVec2(1.0f, flip ? 1.0f : 0.0f),
		ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
		ImVec4(1.0f, 1.0f, 0.0f, 1.0f)
	);
}

void drawTexture2DArray(
	osg::Texture2DArray* texture,
	osg::RenderInfo& ri,
	unsigned int thumbSize
) {
	if(!texture || !ri.getState()) return;

	texture->apply(*ri.getState());

	const int w = texture->getTextureWidth();
	const int h = texture->getTextureHeight();
	const int d = texture->getTextureDepth();
	const auto* obj = texture->getTextureObject(ri.getState()->getContextID());

	if(!w || !h) {
		ImGui::TextDisabled("Array texture has no size.");

		return;
	}

	if(!obj) {
		ImGui::TextDisabled("Array texture object unavailable.");

		return;
	}

	ImGui::Text("%dx%d, %d layer%s  (GL id %u)", w, h, d, d == 1 ? "" : "s", obj->id());
	ImGui::TextDisabled("(array: thumbnail not supported)");
}

namespace {

class FindTexturesVisitor: public osg::NodeVisitor {
public:
	FindTexturesVisitor():
	osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}

	void apply(osg::Node& node) override {
		if(auto* stateset = node.getStateSet()) {
			const auto& textureAttributes = stateset->getTextureAttributeList();

			for(unsigned int unit = 0; unit < textureAttributes.size(); unit++) {
				auto* texture = dynamic_cast<osg::Texture*>(
					stateset->getTextureAttribute(unit, osg::StateAttribute::TEXTURE)
				);

				if(
						texture &&
						std::find(textures.begin(), textures.end(), texture) == textures.end()
				) textures.emplace_back(texture);
			}
		}

		traverse(node);
	}

	std::vector<osg::ref_ptr<osg::Texture>> textures;
};

}

void TextureSection::operator()(osg::RenderInfo& ri) {
	if(!_scanned) refresh(ri);

	if(ImGui::Button("Refresh")) refresh(ri);

	ImGui::SameLine();
	ImGui::Text("Found %d textures", static_cast<int>(_textures.size()));
	ImGui::SliderFloat("Thumbnail size", &_thumbnailSize, 32.0f, 256.0f, "%.0f px");

	if(_textures.empty()) {
		ImGui::TextDisabled("No texture attributes found.");
		return;
	}

	const ImGuiStyle& style = ImGui::GetStyle();
	const float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

	for(std::size_t i = 0; i < _textures.size(); i++) {
		auto* texture = _textures[i].get();
		const std::string name = texture->getName().empty() ? "Texture" : texture->getName();

		ImGui::PushID(texture);
		ImGui::BeginGroup();
		ImGui::TextUnformatted(name.c_str());

		if(auto* t2d = dynamic_cast<osg::Texture2D*>(texture)) drawTexture2D(
			t2d,
			ri,
			static_cast<unsigned int>(_thumbnailSize)
		);

		else if(auto* t2da = dynamic_cast<osg::Texture2DArray*>(texture)) drawTexture2DArray(
			t2da,
			ri,
			static_cast<unsigned int>(_thumbnailSize)
		);

		else ImGui::TextDisabled("(unsupported type)");

		ImGui::EndGroup();

		if(ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::Text("%dx%d", texture->getTextureWidth(), texture->getTextureHeight());

			if(auto* t2d = dynamic_cast<osg::Texture2D*>(texture)) {
				if(auto* image = t2d->getImage()) {
					if(!image->getFileName().empty()) {
						ImGui::TextUnformatted(image->getFileName().c_str());
					}
				}
			}

			ImGui::EndTooltip();
		}

		const float lastX2 = ImGui::GetItemRectMax().x;
		const float nextX2 = lastX2 + style.ItemSpacing.x + _thumbnailSize;

		if(i + 1 < _textures.size() && nextX2 < windowVisibleX2) ImGui::SameLine();

		ImGui::PopID();
	}
}

void TextureSection::refresh(osg::RenderInfo& ri) {
	_textures.clear();
	_scanned = true;

	osg::Node* root = _root.get();

	if(!root && _view.valid()) root = _view->getSceneData();
	if(!root) root = ri.getCurrentCamera();
	if(!root) return;

	FindTexturesVisitor visitor;
	root->accept(visitor);

	_textures = std::move(visitor.textures);
}

StatsSection::StatsSection(osgViewer::Viewer& viewer):
_viewerStats(viewer.getViewerStats()),
_masterCamera(viewer.getCamera()) {
	if(_viewerStats.valid()) {
		_viewerStats->collectStats("frame_rate", true);
		_viewerStats->collectStats("event", true);
		_viewerStats->collectStats("update", true);
		_viewerStats->collectStats("scene", true);
	}

	osgViewer::ViewerBase::Cameras cameras;
	viewer.getCameras(cameras);

	for(auto* camera : cameras) {
		if(auto* stats = camera ? camera->getStats() : nullptr) {
			stats->collectStats("rendering", true);
			stats->collectStats("gpu", true);
			stats->collectStats("scene", true);
		}
	}
}

void StatsSection::operator()(osg::RenderInfo& ri) const {
	auto* viewerStats = _viewerStats.get();
	auto* camera = ri.getCurrentCamera() ? ri.getCurrentCamera() : _masterCamera.get();
	auto* cameraStats = camera ? camera->getStats() : nullptr;

	if(!viewerStats && !cameraStats) {
		ImGui::TextDisabled("No osg::Stats objects available.");
		return;
	}

	if(ImGui::BeginTable("osg_stats", 3, _tableFlags)) {
		ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableSetupColumn("Latest", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableHeadersRow();

		_row(viewerStats, "Frame rate", "FPS", 1.0, true, "%.1f");
		_row(viewerStats, "Event traversal time taken", "Event (ms)", 1000.0, false, "%.3f");
		_row(viewerStats, "Update traversal time taken", "Update (ms)", 1000.0, false, "%.3f");

		if(cameraStats) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled("Camera");

			_row(cameraStats, "Cull traversal time taken", "Cull (ms)", 1000.0, false, "%.3f");
			_row(cameraStats, "Draw traversal time taken", "Draw (ms)", 1000.0, false, "%.3f");
			_row(cameraStats, "GPU draw time taken", "GPU draw (ms)", 1000.0, false, "%.3f");

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled("Visible Scene");

			_latestRow(cameraStats, "Visible number of drawables", "Drawables", "%.0f");
			_latestRow(cameraStats, "Visible number of PrimitiveSets", "PrimitiveSets", "%.0f");
			_latestRow(cameraStats, "Visible vertex count", "Vertices", "%.0f");
			_latestRow(cameraStats, "Visible number of render bins", "Render bins", "%.0f");
		}

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextDisabled("Scene Totals");

		_latestRow(viewerStats, "Number of unique Drawable", "Unique drawables", "%.0f");
		_latestRow(viewerStats, "Number of instanced Drawable", "Instanced drawables", "%.0f");
		_latestRow(viewerStats, "Number of unique Vertices", "Unique vertices", "%.0f");
		_latestRow(viewerStats, "Number of unique Primitives", "Unique primitives", "%.0f");

		ImGui::EndTable();
	}
}

bool StatsSection::_latest(osg::Stats* stats, const std::string& name, double& value) {
	return stats && stats->getAttribute(stats->getLatestFrameNumber(), name, value);
}

void StatsSection::_valueText(bool valid, double value, double multiplier, const char* fmt) {
	if(valid) ImGui::Text(fmt, value * multiplier);
	else ImGui::TextDisabled(".");
}

void StatsSection::_row(
	osg::Stats* stats,
	const std::string& name,
	const char* label,
	double multiplier,
	bool averageInInverseSpace,
	const char* fmt
) {
	double avg = 0.0;
	double latest = 0.0;

	const bool hasAvg = stats && stats->getAveragedAttribute(name, avg, averageInInverseSpace);
	const bool hasLatest = _latest(stats, name, latest);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label);
	ImGui::TableSetColumnIndex(1); _valueText(hasAvg, avg, multiplier, fmt);
	ImGui::TableSetColumnIndex(2); _valueText(hasLatest, latest, multiplier, fmt);
}

void StatsSection::_latestRow(
	osg::Stats* stats,
	const std::string& name,
	const char* label,
	const char* fmt
) {
	double latest = 0.0;
	const bool hasLatest = _latest(stats, name, latest);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label);
	ImGui::TableSetColumnIndex(1); ImGui::TextDisabled(".");
	ImGui::TableSetColumnIndex(2); _valueText(hasLatest, latest, 1.0, fmt);
}

ProfilerSection::ProfilerSection(osgViewer::View& view, osg::Node* sceneRoot, size_t printEvery):
_sceneRoot(sceneRoot) {
	refresh();

	_finalCb = new debug::ProfilerFinalCallback<>(printEvery);

	debug::appendCameraDrawCallback(
		view.getCamera(),
		debug::CameraDrawCallbackSlot::FINAL_DRAW,
		_finalCb
	);
}

void ProfilerSection::refresh() {
	if(!_sceneRoot.valid()) return;

	debug::ProfilerVisitor<> visitor;
	_sceneRoot->accept(visitor);
}

void ProfilerSection::operator()(osg::RenderInfo& ri) {
	const auto& stats = debug::profilerStats(ri.getState()->getContextID());
	const auto frameNum = ri.getState()->getFrameStamp()
		? ri.getState()->getFrameStamp()->getFrameNumber()
		: 0u
	;

	int modeIdx = _finalCb->getMode() == debug::QueryMode::SYNC ? 0 : 1;

	if(ImGui::RadioButton("SYNC", &modeIdx, 0)) _finalCb->setMode(debug::QueryMode::SYNC);

	ImGui::SameLine();

	if(ImGui::RadioButton("ASYNC", &modeIdx, 1)) _finalCb->setMode(debug::QueryMode::ASYNC);

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	if(ImGui::Button("Refresh")) refresh();

	GLuint64 totalNs = 0;

	for(const auto& [path, ps] : stats) {
		const bool unknown = ps.cullState == debug::CullState::UNKNOWN;
		const bool visibleThisFrame = !debug::isCullStale(ps.cullFrame, frameNum) && ps.cullState == debug::CullState::VISIBLE;

		if(unknown || visibleThisFrame) totalNs += ps.gpuBuffer.average();
	}

	constexpr ImGuiTableFlags tableFlags =
		ImGuiTableFlags_Borders |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_SizingStretchProp;

	if(ImGui::BeginTable("gpu_stats", 3, tableFlags)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Drawable Path", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("GPU avg (us)", ImGuiTableColumnFlags_WidthFixed, 72.0f);
		ImGui::TableSetupColumn("% of profiled", ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableHeadersRow();

		for(const auto& [path, ps] : stats) {
			const GLuint64 avgNs = ps.gpuBuffer.average();
			const double avgUs = static_cast<double>(avgNs) / 1000.0;
			const bool culled = ps.cullState != debug::CullState::UNKNOWN
				&& (debug::isCullStale(ps.cullFrame, frameNum) || ps.cullState == debug::CullState::CULLED)
			;
			const float fraction = totalNs > 0
				? static_cast<float>(avgNs) / static_cast<float>(totalNs)
				: 0.0f
			;

			char pctLabel[16];
			std::snprintf(pctLabel, sizeof(pctLabel), "%.1f%%", fraction * 100.0f);

			// ProgressBar rows are taller than plain-text rows (it's a framed
			// widget); AlignTextToFramePadding() lines the plain-text cells up
			// with that height instead of sitting top-left in the extra space.
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(path.c_str());

			if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::AlignTextToFramePadding();

			// Dimmed (not zeroed/blanked) when culled: this is the last measured
			// value, not a live one, but it's not zero-cost either.
			if(culled) ImGui::TextDisabled("%.2f", avgUs);
			else ImGui::Text("%.2f", avgUs);

			ImGui::TableSetColumnIndex(2);

			if(culled) {
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("CULLED");
			}
			else ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), pctLabel);
		}

		ImGui::EndTable();
	}
}

void Panel::draw(osg::RenderInfo& ri) {
	int rem = static_cast<int>(std::count_if(
		_sections.begin(), _sections.end(),
		[](const auto& section) { return section.options.expand; }
	));

	for(auto& section : _sections) {
		const auto forced = _forcedOpen.find(section.label);

		if(forced != _forcedOpen.end()) ImGui::SetNextItemOpen(forced->second, ImGuiCond_Always);

		const bool open = ImGui::CollapsingHeader(
			section.label.c_str(),
			section.options.defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None
		);

		if(section.options.expand) {
			if(open) {
				ImGui::PushID(section.label.c_str());
				ImGui::BeginChild(
					"##expand",
					ImVec2(0.0f, ImGui::GetContentRegionAvail().y / static_cast<float>(rem)),
					ImGuiChildFlags_Borders
				);
				section.fn(ri);
				ImGui::EndChild();
				ImGui::PopID();
			}
			rem--;
		}
		else if(open) section.fn(ri);
	}
}

bool Widget::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) {
	if(!_initialized) {
		osg::Camera* cam = _drawCamera.get();

		if(!cam) {
			if(auto* view = aa.asView()) {
				cam = view->getNumSlaves() > 0
					? view->getSlave(0)._camera.get()
					: view->getCamera()
				;
			}
		}

		if(cam) {
			cam->setPreDrawCallback(new PreDraw(*this));
			cam->setPostDrawCallback(new PostDraw(*this));

			_initialized = true;
		}

		return false;
	}

	if(_firstFrame || ea.getHandled()) return false;

	ImGuiIO& io = ImGui::GetIO();

	switch(ea.getEventType()) {
	case osgGA::GUIEventAdapter::KEYDOWN:
	case osgGA::GUIEventAdapter::KEYUP: {
		bool down = ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN;
		int c = ea.getKey();

		if(io.WantCaptureKeyboard && down && c >= 32 && c < 127) io.AddInputCharacter(
			static_cast<unsigned int>(c)
		);

		return io.WantCaptureKeyboard;
	}

	case osgGA::GUIEventAdapter::PUSH:
		io.AddMousePosEvent(ea.getX(), io.DisplaySize.y - ea.getY());

		if(ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON) io.AddMouseButtonEvent(
			ImGuiMouseButton_Left,
			true
		);

		else if(ea.getButton() == osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON) {
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
		}

		return io.WantCaptureMouse;

	case osgGA::GUIEventAdapter::RELEASE:
		io.AddMousePosEvent(ea.getX(), io.DisplaySize.y - ea.getY());

		if(ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON) io.AddMouseButtonEvent(
			ImGuiMouseButton_Left,
			false
		);

		else if(ea.getButton() == osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON) {
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
		}

		return io.WantCaptureMouse;

	case osgGA::GUIEventAdapter::DRAG:
	case osgGA::GUIEventAdapter::MOVE:
		io.AddMousePosEvent(ea.getX(), io.DisplaySize.y - ea.getY());

		return io.WantCaptureMouse;

	case osgGA::GUIEventAdapter::SCROLL:
		io.AddMouseWheelEvent(
			0.0f,
			ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_UP ? 1.0f : -1.0f
		);

		return io.WantCaptureMouse;

	default:
		break;
	}

	return false;
}

void Widget::newFrame(osg::RenderInfo& ri) {
	if(_firstFrame) {
		ImGui::CreateContext();
		// Widget is deliberately self-contained: do not read/write imgui.ini
		// in whichever directory happens to launch the application. An
		// application-owned ImGui context remains free to enable persistence.
		ImGui::GetIO().IniFilename = nullptr;
		ImGui_ImplOpenGL3_Init();

		_firstFrame = false;
	}

	ImGui_ImplOpenGL3_NewFrame();

	// A nested (non-slave) drawCamera - e.g. an app's own downstream
	// POST_RENDER compositing camera, see the Widget constructor comment --
	// generally never gets an explicit GraphicsContext of its own; fall back
	// to the viewer's master camera, which always has the real one.
	auto* gc = ri.getCurrentCamera()->getGraphicsContext();

	if(!gc && _masterCamera.valid()) gc = _masterCamera->getGraphicsContext();

	auto* traits = gc->getTraits();

	ImGuiIO& io = ImGui::GetIO();

	io.DisplaySize = ImVec2(
		static_cast<float>(traits->width),
		static_cast<float>(traits->height)
	);

	double t = ri.getView()->getFrameStamp()->getSimulationTime();

	io.DeltaTime = static_cast<float>(t - _time) + 0.0000001f;

	_time = t;

	ImGui::NewFrame();
}

void Widget::render(osg::RenderInfo& ri) {
	ImGuiWindowFlags flags = ImGuiWindowFlags_None;

	if(_opts.dock == Dock::NONE) ImGui::SetNextWindowSize(
		ImVec2(600, 320),
		ImGuiCond_FirstUseEver
	);

	else {
		const ImGuiIO& io = ImGui::GetIO();
		float x = _opts.dock == Dock::LEFT ? 0.0f : io.DisplaySize.x - _opts.dockWidth;

		ImGui::SetNextWindowPos(ImVec2(x, 0.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(_opts.dockWidth, io.DisplaySize.y), ImGuiCond_Always);

		// Keep a docked panel pinned and fixed-width, but leave Dear ImGui's
		// standard title-bar collapse button enabled. A comparison viewport
		// needs a quick way to clear the overlay without losing its controls:
		// click the title-bar triangle to collapse it, then click again to reopen.
		flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
	}

	if(ImGui::Begin("osgx::imgui", nullptr, flags)) {
		if(_opts.showGPUInfo) {
			static const auto* glRenderer = glGetString(GL_RENDERER);
			static const auto* glVersion = glGetString(GL_VERSION);
			static const auto* glVendor = glGetString(GL_VENDOR);

			if(glVendor) ImGui::TextDisabled("Vendor: %s", glVendor);
			if(glRenderer) ImGui::TextDisabled("Renderer: %s", glRenderer);
			if(glVersion) ImGui::TextDisabled("GL: %s", glVersion);

			ImGui::Separator();
		}

		if(_opts.showFrameInfo) {
			auto* fs = ri.getView()->getFrameStamp();

			ImGui::Text("Frame: %u", fs->getFrameNumber());
			ImGui::Text("Time: %.3f s", fs->getSimulationTime());

			ImGui::Separator();
		}

		_panel.draw(ri);
	}

	ImGui::End();
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

}

#endif
