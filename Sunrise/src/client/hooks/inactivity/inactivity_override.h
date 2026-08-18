#pragma once

#include <cstdint>

namespace sunrise::client::hooks::inactivity {

/** What the override reached, which is what a lane not taking says. */
struct Status {
    /** Address of the activity config object, or zero until the Client publishes one. */
    std::uintptr_t address{};
    /** Set once the config getter has been found in the image. */
    bool resolved{};
    /** Set once the Client's own lanes have been read back. */
    bool captured{};
};

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
