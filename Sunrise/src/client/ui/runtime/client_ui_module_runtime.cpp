#include "client_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../movement/movement_panel.h"

namespace sunrise::client::ui::runtime {
namespace {

/** Namespaced stable ID prevents Client modules from colliding with Server modules. */
constexpr std::string_view kMovementStableId = "client.movement";
/** Short menu label for the shared teleport and noclip page. */
constexpr std::string_view kMovementDisplayName = "Movement";

core::ui::modules::registry::PageRegistration g_movementPage;

} // namespace

/** @return True when the Client module owns its Core UI registry slot. */
bool initialize() noexcept {
    return g_movementPage.acquire(
        core::ui::modules::Owner::client, kMovementStableId, kMovementDisplayName, &movement::draw);
}

/** Removes the Client module from the Core UI registry. */
void shutdown() noexcept {
    g_movementPage.release();
}

} // namespace sunrise::client::ui::runtime
