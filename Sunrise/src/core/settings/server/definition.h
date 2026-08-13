#pragma once

#include "../../../state/entitlements/definition.h"

namespace sunrise::core::settings::server {

/** Read-only Server settings. */
struct Settings {
    /** Authored ownership policy declared by SignOn and defined by the content manifest. */
    state::entitlements::Table entitlements{};
};

} // namespace sunrise::core::settings::server
