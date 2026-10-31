#include "osgx/ImGui.hpp"

#ifdef OSGX_IMGUI

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>

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

std::pair<bool, int> combo(
	const std::string& label,
	int value,
	const std::vector<std::string>& labels
) {
	std::vector<const char*> items;

	items.reserve(labels.size());

	for(const auto& l : labels) items.push_back(l.c_str());

	const bool changed = ImGui::Combo(
		label.c_str(),
		&value,
		items.data(),
		static_cast<int>(items.size())
	);

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

ProfilerSection::ProfilerSection(osgViewer::View& view):
_profiler(new debug::Profiler()) {
	if(auto* viewer = view.getViewerBase()) _profiler->attach(*viewer);
}

void ProfilerSection::refresh() {
	_profiler->refresh();
}

namespace {

constexpr std::array<double, 5> BUDGET_HZ{30.0, 60.0, 90.0, 120.0, 144.0};

constexpr const char* BUDGET_LABELS[]{
	"30 Hz (33.3 ms)",
	"60 Hz (16.7 ms)",
	"90 Hz (11.1 ms)",
	"120 Hz (8.3 ms)",
	"144 Hz (6.9 ms)"
};

constexpr ImU32 GPU_COLOR = IM_COL32(230, 90, 140, 255);
constexpr ImU32 WAIT_COLOR = IM_COL32(130, 130, 130, 150);
constexpr ImU32 OVER_BUDGET_TINT = IM_COL32(235, 55, 55, 110);
constexpr ImU32 OVER_BUDGET_OUTLINE = IM_COL32(235, 55, 55, 255);
constexpr ImU32 BUDGET_LINE_COLOR = IM_COL32(235, 55, 55, 170);

const ImVec4 OVER_BUDGET_TEXT(0.92f, 0.22f, 0.22f, 1.0f);
const ImVec4 NEAR_BUDGET_TEXT(0.95f, 0.70f, 0.20f, 1.0f);

// The share of the budget from which a busy time is shown as near it (amber).
constexpr double NEAR_BUDGET = 0.9;

ImU32 phaseColor(debug::Phase phase) {
	switch(phase) {
		case debug::Phase::EVENT: return IM_COL32(170, 130, 230, 255);
		case debug::Phase::UPDATE: return IM_COL32(90, 150, 240, 255);
		case debug::Phase::CULL: return IM_COL32(100, 200, 115, 255);
		case debug::Phase::DRAW: return IM_COL32(240, 165, 75, 255);
	}

	return IM_COL32_WHITE;
}

struct Waterfall {
	unsigned int frame;
	double axisMs;
	double budgetMs;
	double minMs;

	// Per row (see waterfallKey()): whether it was shown, for the threshold's hysteresis.
	std::unordered_map<std::string, bool>* shown;
};

std::string waterfallKey(const debug::TraceNode& node) {
	return node.label + "@" + std::to_string(reinterpret_cast<std::uintptr_t>(node.object)) + "/"
		+ debug::detail::toString(node.phase);
}

// Whether `node` gets a row: it ran in the frame, was not culled, and its CPU or GPU ms reaches
// the threshold - or, if it was shown, half of it (hysteresis: a node whose time hovers around
// the threshold does not blink in and out). Calling it again in the same frame gives the same
// answer.
bool waterfallVisible(const debug::TraceNode& node, const Waterfall& waterfall) {
	if(node.frame != waterfall.frame || node.culled) return false;

	const auto cpu = debug::cpuSpan(node, waterfall.frame);

	if(!cpu) return false;

	const auto gpu = debug::gpuSpan(node, waterfall.frame);
	const double ms = std::max(cpu->ms, gpu ? gpu->ms : 0.0);

	bool& shown = (*waterfall.shown)[waterfallKey(node)];

	shown = ms >= (shown ? waterfall.minMs * 0.5 : waterfall.minMs);

	return shown;
}

// `label` and `ms`, amber from NEAR_BUDGET of the budget and red over it.
void budgetText(const char* label, double ms, double budgetMs) {
	if(ms > budgetMs) ImGui::TextColored(OVER_BUDGET_TEXT, "%s %.2f ms", label, ms);

	else if(ms >= budgetMs * NEAR_BUDGET) {
		ImGui::TextColored(NEAR_BUDGET_TEXT, "%s %.2f ms", label, ms);
	}

	else ImGui::Text("%s %.2f ms", label, ms);
}

// A bar over [y0, y1] from `span`'s start to its end, in the cell starting at x `left`, `width`
// wide; unless `waiting`, the part past the budget is tinted and outlined red.
void waterfallBar(
	ImDrawList* drawList,
	float left,
	float width,
	float y0,
	float y1,
	const debug::TraceSpan& span,
	ImU32 color,
	bool waiting,
	const Waterfall& waterfall
) {
	auto x = [&](double ms) {
		return left + width * static_cast<float>(std::clamp(ms / waterfall.axisMs, 0.0, 1.0));
	};

	const float x0 = x(span.startMs);
	const float x1 = std::max(x(span.endMs), x0 + 1.0f);
	const float budget = x(waterfall.budgetMs);

	drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color);

	if(x1 > budget && !waiting) {
		const ImVec2 overMin(std::max(x0, budget), y0);
		const ImVec2 overMax(x1, y1);

		drawList->AddRectFilled(overMin, overMax, OVER_BUDGET_TINT);
		drawList->AddRect(overMin, overMax, OVER_BUDGET_OUTLINE);
	}
}

void waterfallTooltip(
	const debug::TraceNode& node,
	const debug::TraceSpan& cpu,
	const std::optional<debug::TraceSpan>& gpu
) {
	ImGui::BeginTooltip();
	ImGui::TextUnformatted(node.label.c_str());

	const char* phase = debug::detail::toString(node.phase);

	if(node.className) ImGui::TextDisabled("%s, %s", node.className, phase);

	else ImGui::TextDisabled("%s", phase);

	ImGui::Separator();
	ImGui::Text("CPU: %.3f ms, at %.3f ms", cpu.ms, cpu.startMs);

	if(node.location == &debug::detail::profilerGroupLocation()) {
		ImGui::TextDisabled("the profiler's own work (fold) and wait (readback), not the app's");
	}

	if(debug::detail::isWaitLocation(node.location)) {
		ImGui::TextDisabled("a wait, not counted as CPU work (swap: for the display with vsync;");
		ImGui::TextDisabled("readback: for the GPU, or where the driver blocks for vsync)");
	}

	if(node.count > 0.0) ImGui::Text("self %.3f ms, %.1f spans per frame", node.selfMs, node.count);

	if(gpu) {
		ImGui::Text("GPU: %.3f ms (red: drawn from the row's start, as cost)", gpu->ms);
		ImGui::TextDisabled(
			"the GPU ran it %.3f ms after the CPU started it",
			gpu->startMs - cpu.startMs
		);
	}

	if(node.draws > 0.0) {
		ImGui::Text("draws per frame: %.1f, first in draw order: %d", node.draws, node.drawOrder);
	}

	ImGui::TextDisabled("moving averages, frame %u", node.frame);
	ImGui::EndTooltip();
}

// The row of `node` and, if open, its visible children's rows.
void waterfallRow(const debug::TraceNode& node, const Waterfall& waterfall, bool defaultOpen) {
	const auto cpu = debug::cpuSpan(node, waterfall.frame);
	const auto gpu = debug::gpuSpan(node, waterfall.frame);
	const bool waiting =
		debug::detail::isWaitLocation(node.location) ||
		node.location == &debug::detail::profilerGroupLocation()
	;

	const bool hasChildren = std::any_of(
		node.children.begin(),
		node.children.end(),
		[&waterfall](const debug::TraceNode& child) { return waterfallVisible(child, waterfall); }
	);

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns;

	if(!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

	if(defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);

	// Siblings with the same label (one name on several nodes) are told apart by their object.
	ImGui::PushID(node.object);

	const bool open = ImGui::TreeNodeEx(node.label.c_str(), flags);

	if(ImGui::IsItemHovered() && cpu) waterfallTooltip(node, *cpu, gpu);

	ImGui::TableSetColumnIndex(1);

	if(cpu) ImGui::Text("%.3f", cpu->ms);

	ImGui::TableSetColumnIndex(2);

	if(gpu) ImGui::Text("%.3f", gpu->ms);

	ImGui::TableSetColumnIndex(3);

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float width = ImGui::GetContentRegionAvail().x;
	const float height = ImGui::GetTextLineHeight();
	const float pad = ImGui::GetStyle().CellPadding.y;
	auto* drawList = ImGui::GetWindowDrawList();

	if(cpu) {
		waterfallBar(
			drawList,
			origin.x,
			width,
			origin.y + 1.0f,
			origin.y + height,
			*cpu,
			waiting ? WAIT_COLOR : phaseColor(node.phase),
			waiting,
			waterfall
		);
	}

	// The GPU time of the row's draws overlays the CPU bar from its start (below a band that keeps
	// the CPU bar's length visible), running past it when the GPU takes longer.
	if(cpu && gpu) {
		const debug::TraceSpan cost{cpu->startMs, cpu->startMs + gpu->ms, gpu->ms};

		waterfallBar(
			drawList,
			origin.x,
			width,
			origin.y + height * 0.35f,
			origin.y + height,
			cost,
			GPU_COLOR,
			false,
			waterfall
		);
	}

	if(waterfall.budgetMs < waterfall.axisMs) {
		const auto share = static_cast<float>(waterfall.budgetMs / waterfall.axisMs);
		const float x = origin.x + width * share;

		drawList->AddLine(
			ImVec2(x, origin.y - pad),
			ImVec2(x, origin.y + height + pad),
			BUDGET_LINE_COLOR
		);
	}

	ImGui::Dummy(ImVec2(width, height));

	if(open && hasChildren) {
		for(const auto& child : node.children) {
			if(waterfallVisible(child, waterfall)) waterfallRow(child, waterfall, false);
		}

		ImGui::TreePop();
	}

	ImGui::PopID();
}

}

void ProfilerSection::operator()(osg::RenderInfo& ri) {
	int modeIdx = _profiler->getMode() == debug::QueryMode::SYNC ? 0 : 1;

	if(ImGui::RadioButton("SYNC", &modeIdx, 0)) _profiler->setMode(debug::QueryMode::SYNC);

	ImGui::SameLine();

	if(ImGui::RadioButton("ASYNC", &modeIdx, 1)) _profiler->setMode(debug::QueryMode::ASYNC);

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	if(ImGui::Button("Refresh")) refresh();

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	const float comboWidth = ImGui::CalcTextSize(BUDGET_LABELS[0]).x + ImGui::GetFrameHeight() * 2.0f;

	ImGui::SetNextItemWidth(comboWidth);
	ImGui::Combo("budget", &_budget, BUDGET_LABELS, static_cast<int>(BUDGET_HZ.size()));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::CalcTextSize("0.000").x + ImGui::GetFrameHeight());
	ImGui::DragFloat("hide below ms", &_minMs, 0.005f, 0.0f, 5.0f, "%.3f");

	const auto newest = _profiler->getProfileFrame();

	if(!newest) {
		ImGui::TextDisabled("no frames yet");

		return;
	}

	const auto profile = _profiler->getProfile();
	const double budgetMs = 1000.0 / BUDGET_HZ[static_cast<std::size_t>(_budget)];

	const auto load = debug::frameLoad(profile, *newest);

	// Forgets rows not seen in a long while.
	if(_shown.size() > 4096) _shown.clear();

	const Waterfall waterfall{
		*newest,
		std::max(budgetMs, load.endMs),
		budgetMs,
		static_cast<double>(_minMs),
		&_shown
	};

	// The budget is compared with busy times; waits (swap, readback) are not work.
	ImGui::Text("frame %u:", *newest);
	ImGui::SameLine();
	budgetText("CPU", load.cpuMs, budgetMs);
	ImGui::SameLine();
	budgetText("GPU", load.gpuMs, budgetMs);
	ImGui::SameLine();
	ImGui::Text("of the %.2f ms budget", budgetMs);
	ImGui::SameLine();
	ImGui::TextDisabled("(red = a row's GPU time, drawn from its start)");

	constexpr ImGuiTableFlags tableFlags =
		ImGuiTableFlags_BordersInnerV |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_SizingStretchProp;

	if(ImGui::BeginTable("waterfall", 4, tableFlags)) {
		char axis[48];

		std::snprintf(axis, sizeof(axis), "0 - %.1f ms", waterfall.axisMs);

		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("node", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		const float msWidth = ImGui::CalcTextSize("00.000").x;

		ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed, msWidth);
		ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, msWidth);
		ImGui::TableSetupColumn(axis, ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableHeadersRow();

		for(const auto& phase : profile) {
			if(waterfallVisible(phase, waterfall)) waterfallRow(phase, waterfall, true);
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

namespace {

// The ImGuiKey of an osgGA key: editing, navigation and modifier keys, and letters (for shortcuts
// such as Ctrl+A/C/V; with Ctrl held, OSG may report a letter as its control code 1-26).
ImGuiKey imguiKey(int key, bool ctrl) {
	using Key = osgGA::GUIEventAdapter::KeySymbol;

	switch(key) {
		case Key::KEY_BackSpace: return ImGuiKey_Backspace;
		case Key::KEY_Delete: return ImGuiKey_Delete;
		case Key::KEY_Return: return ImGuiKey_Enter;
		case Key::KEY_KP_Enter: return ImGuiKey_KeypadEnter;
		case Key::KEY_Tab: return ImGuiKey_Tab;
		case Key::KEY_Escape: return ImGuiKey_Escape;
		case Key::KEY_Left: return ImGuiKey_LeftArrow;
		case Key::KEY_Right: return ImGuiKey_RightArrow;
		case Key::KEY_Up: return ImGuiKey_UpArrow;
		case Key::KEY_Down: return ImGuiKey_DownArrow;
		case Key::KEY_Home: return ImGuiKey_Home;
		case Key::KEY_End: return ImGuiKey_End;
		case Key::KEY_Page_Up: return ImGuiKey_PageUp;
		case Key::KEY_Page_Down: return ImGuiKey_PageDown;
		case Key::KEY_Insert: return ImGuiKey_Insert;
		case Key::KEY_Control_L: return ImGuiKey_LeftCtrl;
		case Key::KEY_Control_R: return ImGuiKey_RightCtrl;
		case Key::KEY_Shift_L: return ImGuiKey_LeftShift;
		case Key::KEY_Shift_R: return ImGuiKey_RightShift;
		case Key::KEY_Alt_L: return ImGuiKey_LeftAlt;
		case Key::KEY_Alt_R: return ImGuiKey_RightAlt;
		case Key::KEY_Super_L: return ImGuiKey_LeftSuper;
		case Key::KEY_Super_R: return ImGuiKey_RightSuper;
		default: break;
	}

	if(key >= 'a' && key <= 'z') return static_cast<ImGuiKey>(ImGuiKey_A + (key - 'a'));

	if(key >= 'A' && key <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (key - 'A'));

	if(ctrl && key >= 1 && key <= 26) return static_cast<ImGuiKey>(ImGuiKey_A + (key - 1));

	return ImGuiKey_None;
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

		const auto mods = static_cast<unsigned int>(ea.getModKeyMask());
		const bool ctrl = mods & osgGA::GUIEventAdapter::MODKEY_CTRL;

		io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
		io.AddKeyEvent(ImGuiMod_Shift, mods & osgGA::GUIEventAdapter::MODKEY_SHIFT);
		io.AddKeyEvent(ImGuiMod_Alt, mods & osgGA::GUIEventAdapter::MODKEY_ALT);
		io.AddKeyEvent(ImGuiMod_Super, mods & osgGA::GUIEventAdapter::MODKEY_SUPER);

		if(const auto key = imguiKey(c, ctrl); key != ImGuiKey_None) io.AddKeyEvent(key, down);

		if(io.WantCaptureKeyboard && down && !ctrl && c >= 32 && c < 127) io.AddInputCharacter(
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
