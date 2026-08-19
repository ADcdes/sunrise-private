#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::client::inactivity {

/** Activity lanes the Client keeps a separate inactivity timeout for. */
inline constexpr std::size_t kActivityCount = 14;

/**
 * Shortest timeout offered, in milliseconds.
 *
 * The Client will not time any lane out until the session has outlived its own grace, which is
 * around a minute on this build and which this module does not write. A shorter lane could not
 * fire any sooner, so offering one would only look like a hold that is not working. A file
 * carrying a smaller value is clamped up to this rather than refused.
 */
inline constexpr std::uint32_t kMinimumTimeoutMs = 60000;
/** Longest timeout offered, in milliseconds. A day outlasts any session. */
inline constexpr std::uint32_t kMaximumTimeoutMs = 86400000;

/**
 * The orbit lane.
 *
 * A timeout that fires in orbit drops the session, and this Client cannot establish another one:
 * the next screen is a marrionberry error and the process has to be restarted. The lane is held
 * at its longest whenever the hold is on, and no field or file value reaches it.
 */
inline constexpr std::size_t kOrbitLane = 13;

/** One activity lane's names and its key in the stored document. */
struct ActivityInfo {
    /** Full name, shown where there is room for it. */
    std::string_view name;
    /** Column heading, short enough for fourteen lanes across one grid. */
    std::string_view column;
    /** Key this lane is stored under. */
    std::string_view key;
};

/** Every lane, in block order. */
inline constexpr std::array<ActivityInfo, kActivityCount> kActivities{{
    {"PvE", "PvE", "pve"},
    {"PvE (guided)", "PvE gd", "pve_guided"},
    {"PvE (matchmade, multiple fireteams)", "PvE mm+", "pve_mm_multiple"},
    {"PvE (matchmade, single fireteam)", "PvE mm", "pve_mm_single"},
    {"PvE (special)", "PvE sp", "pve_special"},
    {"PvP", "PvP", "pvp"},
    {"PvP (guided)", "PvP gd", "pvp_guided"},
    {"PvP (matchmade, multiple fireteams)", "PvP mm+", "pvp_mm_multiple"},
    {"PvP (matchmade, single fireteam)", "PvP mm", "pvp_mm_single"},
    {"PvP (special)", "PvP sp", "pvp_special"},
    {"PvP (private)", "Private", "pvp_private"},
    {"PvP (Trials)", "Trials", "pvp_trials"},
    {"Social", "Social", "social"},
    {"Orbit", "Orbit", "orbit"},
}};

/** @return Every lane at its longest. */
[[nodiscard]] consteval std::array<std::uint32_t, kActivityCount> longest_timeouts() noexcept {
    std::array<std::uint32_t, kActivityCount> values{};
    values.fill(kMaximumTimeoutMs);
    return values;
}

/** Compiled lanes a fresh install holds. */
inline constexpr std::array<std::uint32_t, kActivityCount> kDefaultTimeouts = longest_timeouts();


/**
 * Runtime inactivity configuration. This module owns it; Core settings do not carry it.
 *
 * The two switches are exclusive, because they describe opposite behaviour: one removes every
 * timeout and the other replaces each with a chosen one. Neither set leaves the Client's own
 * timeouts in place.
 */
struct Settings {
    /** Milliseconds per lane, in block order. Held only while custom is set. */
    std::array<std::uint32_t, kActivityCount> timeouts{kDefaultTimeouts};
    bool enabled{false};
    /** Never set alongside enabled; the two ask for opposite things. */
    bool custom{false};
};

/**
 * Resolves the configuration file and loads it when one exists.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 */
void initialize(void* module) noexcept;

/** Drops the runtime configuration and the resolved file path. */
void shutdown() noexcept;

/** @return One lock-consistent copy of the current configuration. */
[[nodiscard]] Settings get() noexcept;

/**
 * Publishes one configuration and writes it straight to disk.
 * @param settings Candidate configuration, refused when a lane is out of range.
 * @return True when the value was published. A failed write is logged, not returned.
 */
bool publish(const Settings& settings) noexcept;

} // namespace sunrise::client::inactivity
