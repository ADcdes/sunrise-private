/** The player module's interface. Every control saves at once, so a change survives a restart. */

#include "player_panel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../hooks/inactivity/inactivity_override.h"
#include "../../inactivity/inactivity_settings_store.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::ui::player {
namespace {

namespace inactivity = client::inactivity;
namespace toggle = core::ui::components::toggle;

constexpr int kLaneColumns = 7;

/**
 * Draws one lane's field.
 * @param index Lane in block order.
 * @param configured Configuration updated on an edit.
 * @return True when this lane changed.
 */
[[nodiscard]] bool draw_lane(std::size_t index, inactivity::Settings& configured) noexcept {
    const bool orbit = index == inactivity::kOrbitLane;
    bool changed = false;
    ImGui::PushID(static_cast<int>(index));
    ImGui::BeginDisabled(orbit);
    ImGui::TextUnformatted(inactivity::kActivities[index].column.data());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", inactivity::kActivities[index].name.data());
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
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        configured.timeouts[index] =
            std::clamp(milliseconds, inactivity::kMinimumTimeoutMs, inactivity::kMaximumTimeoutMs);
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
void draw_inactivity_status(const hooks::inactivity::Status& status, bool enabled) noexcept {
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

void draw_inactivity() noexcept {
    inactivity::Settings configured = inactivity::get();

    ImGui::TextUnformatted("Inactivity");
    ImGui::Separator();
    ImGui::TextWrapped("Disable AFK timeouts from activities kicking to orbit and the title "
                       "screen.");
    ImGui::Spacing();

    bool changed = toggle::control("Enabled##inactivity", configured.enabled);
    draw_inactivity_status(hooks::inactivity::status(), configured.enabled);

    if (ImGui::CollapsingHeader("Advanced##inactivity")) {
        changed =
            toggle::control("Use set timeouts##inactivity_custom", configured.custom) || changed;
        ImGui::TextDisabled("Milliseconds per activity, the unit the client's own inactivity "
                            "overlay prints. Taken when a field is left.");
        ImGui::Spacing();
        ImGui::BeginDisabled(!configured.custom);
        if (ImGui::BeginTable("lanes", kLaneColumns, ImGuiTableFlags_SizingStretchSame)) {
            for (std::size_t index = 0; index < inactivity::kActivityCount; ++index) {
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
        (void)inactivity::publish(configured);
    }
}

} // namespace

/** Draws the player module inside the active Core UI frame. */
void draw() noexcept {
    client::player::Settings settings = client::player::get();

    ImGui::TextUnformatted("Infinite Ammo");
    ImGui::Separator();
    ImGui::TextWrapped("Keep every weapon's reserves full.");
    ImGui::Spacing();

    const bool changed = core::ui::components::toggle::control("Enabled##infinite_ammo",
                                                               settings.infiniteAmmoEnabled);
    if (changed) {
        (void)client::player::publish(settings);
    }

    ImGui::Spacing();
    ImGui::Spacing();
    draw_inactivity();
}

} // namespace sunrise::client::ui::player
