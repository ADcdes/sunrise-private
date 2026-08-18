/**
 * The inactivity section. The switch on its own is the whole feature for most operators, so the
 * per-lane milliseconds sit behind an advanced header and a switch of their own. Every control
 * saves at once, so a change survives a restart.
 */

#include "inactivity_panel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../hooks/inactivity/inactivity_override.h"
#include "../../inactivity/inactivity_settings_store.h"

namespace sunrise::client::ui::inactivity {
namespace {

namespace settings = client::inactivity;
namespace toggle = core::ui::components::toggle;

/** Lanes per row in the advanced grid. Seven lays the fourteen out in two even rows. */
constexpr int kLaneColumns = 7;

/**
 * Draws one lane's field.
 * @param index Lane in block order.
 * @param configured Configuration updated on an edit.
 * @return True when this lane changed.
 */
[[nodiscard]] bool draw_lane(std::size_t index, settings::Settings& configured) noexcept {
    const bool orbit = index == settings::kOrbitLane;
    bool changed = false;
    ImGui::PushID(static_cast<int>(index));
    ImGui::BeginDisabled(orbit);
    ImGui::TextUnformatted(settings::kActivities[index].column.data());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", settings::kActivities[index].name.data());
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::uint32_t milliseconds = configured.timeouts[index];
    ImGui::InputScalar("##lane",
                       ImGuiDataType_U32,
                       &milliseconds,
                       nullptr,
                       nullptr,
                       "%u",
                       ImGuiInputTextFlags_CharsDecimal);
    // Taken when the field is left rather than on each keystroke, so a half-typed number is
    // never clamped out from under the caret.
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        configured.timeouts[index] =
            std::clamp(milliseconds, settings::kMinimumTimeoutMs, settings::kMaximumTimeoutMs);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

/**
 * @param status What the override reached.
 * @param enabled Whether the hold is switched on.
 */
void draw_status(const client::hooks::inactivity::Status& status, bool enabled) noexcept {
    if (!status.resolved) {
        ImGui::TextDisabled("Timeouts not reachable in this build.");
    } else if (status.address == 0 || !status.captured) {
        ImGui::TextDisabled("Waiting for an activity to load.");
    } else if (enabled) {
        ImGui::TextDisabled("Holding this session's timeouts.");
    } else {
        ImGui::TextDisabled("Using the client's own timeouts.");
    }
}

} // namespace

/** Draws the inactivity section inside whichever page hosts it. */
void draw_section() noexcept {
    settings::Settings configured = settings::get();

    ImGui::TextUnformatted("Inactivity");
    ImGui::Separator();
    ImGui::TextWrapped("The client returns a session to orbit once its controller has been idle "
                       "for the timeout of the activity it is in.");
    ImGui::Spacing();

    bool changed = toggle::control("Enabled##inactivity", configured.enabled);
    draw_status(client::hooks::inactivity::status(), configured.enabled);

    if (ImGui::CollapsingHeader("Advanced##inactivity")) {
        changed =
            toggle::control("Use set timeouts##inactivity_custom", configured.custom) || changed;
        ImGui::TextDisabled("Milliseconds per activity, the unit the client's own inactivity "
                            "overlay prints. Taken when a field is left.");
        ImGui::Spacing();
        ImGui::BeginDisabled(!configured.custom);
        if (ImGui::BeginTable("lanes", kLaneColumns, ImGuiTableFlags_SizingStretchSame)) {
            for (std::size_t index = 0; index < settings::kActivityCount; ++index) {
                ImGui::TableNextColumn();
                changed = draw_lane(index, configured) || changed;
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextDisabled("Orbit is held but not editable; a timeout there needs a restart.");
        ImGui::PopTextWrapPos();
    }

    if (changed) {
        (void)settings::publish(configured);
    }
}

} // namespace sunrise::client::ui::inactivity
