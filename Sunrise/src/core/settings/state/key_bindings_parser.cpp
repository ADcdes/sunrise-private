#include <algorithm>
#include <array>
#include <limits>

#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

namespace bindings = state::account::settings::bindings;

/** Stable JSON names follow the fixed authored action order in State. */
constexpr std::array<std::string_view, bindings::kActionCount> kActionNames{
    "fire",
    "toggle_zoom",
    "hold_zoom",
    "melee",
    "grenade",
    "super",
    "reload",
    "light_attack",
    "heavy_attack",
    "block",
    "switch_weapons",
    "next_weapon",
    "previous_weapon",
    "primary_weapon",
    "special_weapon",
    "heavy_weapon",
    "move_forward",
    "move_backward",
    "move_left",
    "move_right",
    "jump",
    "toggle_crouch",
    "hold_crouch",
    "toggle_sprint",
    "hold_sprint",
    "vehicle_boost",
    "vehicle_brake",
    "vehicle_zoom",
    "vehicle_fire_primary",
    "vehicle_fire_secondary",
    "vehicle_exit",
    "interact",
    "highlight_player",
    "emote_1",
    "emote_2",
    "emote_3",
    "emote_4",
    "air_move",
    "class_ability",
    "death_cam_zoom_in",
    "death_cam_zoom_out",
    "push_to_talk",
    "ui_gamepad_button_back",
    "ui_open_director",
    "ui_open_director_store_tab",
    "ui_open_director_pursuits_tab",
    "ui_open_director_map_tab",
    "ui_open_director_destinations_tab",
    "ui_open_director_roster_tab",
    "ui_open_director_seasons_tab",
    "ui_open_start_menu_alternative",
    "ui_open_start_menu_records_tab",
    "ui_open_start_menu_collections_tab",
    "ui_open_start_menu_clan_tab",
    "ui_open_start_menu_inventory_tab",
    "ui_open_start_menu_settings_tab",
    "ui_open_exit_dialog_confirm",
    "ui_abort_activity",
    "ui_text_chat_toggle_state",
    "screenshot",
};

/**
 * Finds one action name without building a map at boot.
 * @param name Borrowed JSON property name.
 * @return Fixed State index, or the action count when the name is unknown.
 */
[[nodiscard]] std::size_t action_index(std::string_view name) noexcept {
    const auto found = std::find(kActionNames.begin(), kActionNames.end(), name);
    return static_cast<std::size_t>(found - kActionNames.begin());
}

} // namespace

/** Parses the whole fixed action table under named JSON properties. */
bool Parser::key_bindings(bindings::KeyBindings& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    std::array<bool, bindings::kActionCount> present{};
    std::size_t presentCount = 0;
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        const std::size_t index = action_index(key);
        if (index == bindings::kActionCount) {
            if (!skip_value(0)) {
                return false;
            }
        } else {
            if (present[index] || !key_binding(output.values[index])) {
                return false;
            }
            present[index] = true;
            ++presentCount;
        }
        if (consume('}')) {
            output.configured = presentCount == bindings::kActionCount;
            return output.configured;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses one binding with explicit primary and secondary halves. */
bool Parser::key_binding(bindings::Binding& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    bool hasPrimary = false;
    bool hasSecondary = false;
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "primary") {
            if (hasPrimary || !optional_input_code(output.primary)) {
                return false;
            }
            hasPrimary = true;
        } else if (key == "secondary") {
            if (hasSecondary || !optional_input_code(output.secondary)) {
                return false;
            }
            hasSecondary = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return hasPrimary && hasSecondary;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses one optional 16-bit input code. Lossy JSON numbers are refused. */
bool Parser::optional_input_code(std::optional<std::uint16_t>& output) noexcept {
    if (literal("null")) {
        output.reset();
        return true;
    }
    std::uint64_t value = 0;
    if (!unsigned_integer(value) || value > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint16_t>(value);
    return true;
}

} // namespace sunrise::core::settings::parser
