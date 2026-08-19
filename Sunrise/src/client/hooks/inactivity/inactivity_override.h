#pragma once

#include <array>
#include <cstdint>

#include "../../inactivity/inactivity_settings_store.h"

namespace sunrise::client::hooks::inactivity {

/** What the override reached, which is what a lane not taking says. */
struct Status {
    /** Read back rather than assumed, so a hold that never reached the Client still reads true. */
    std::array<std::uint32_t, client::inactivity::kActivityCount> live{};
    /** Address of the activity config object, or zero until the Client publishes one. */
    std::uintptr_t address{};
    /** Set once the config getter has been found in the image. */
    bool resolved{};
    /** Set once the Client's own lanes have been read back. */
    bool captured{};
    bool liveValid{};
    /** Zero is meaningful: it is the value that stops the Client gating on it at all. */
    std::uint32_t liveGraceMs{};
    bool liveGraceValid{};
};

/**
 * The Client's own two clocks, read through the same getters it uses.
 *
 * A lane times out when idle passes its milliseconds, and nothing times out at all until the
 * session passes the grace.
 */
struct Timers {
    /** Input resets this, so it does not track the session and the two can diverge widely. */
    std::uint64_t idleMs{};
    std::uint64_t sessionMs{};
    bool resolved{};
    bool idleValid{};
    bool sessionValid{};
};

/**
 * Reads the Client's idle and session clocks.
 *
 * Every call enters Client code, so this is deliberately kept out of poll(): a caller pays for it
 * only while it is displaying the result, and nothing pays for it otherwise. A caller that draws
 * every frame does call it every frame. It writes nothing, and reports nothing when install could
 * not resolve the getters.
 * @return The clocks, with a validity flag for each.
 */
[[nodiscard]] Timers timers() noexcept;

/**
 * Resolves the activity config getter, which the lanes are reached through.
 * @return True when it was found.
 */
[[nodiscard]] bool install() noexcept;

/** Puts the Client's own lanes back and drops the resolved getter. */
void uninstall() noexcept;

/**
 * Holds the configured milliseconds in the activity config object, or puts back the ones the
 * Client authored. Call once a frame from any steady tick.
 */
void poll() noexcept;

/** @return A consistent copy of what the override reached. */
[[nodiscard]] Status status() noexcept;

} // namespace sunrise::client::hooks::inactivity
