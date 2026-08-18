/**
 * Inactivity timeout override.
 *
 * The Client keeps one timeout per activity lane and ends a session whose controller has been
 * idle for longer. The lanes are not image data: they sit at a fixed offset inside a live object,
 * and the pointer to that object is stored obfuscated, so the Client reaches it through a getter
 * that decodes the pointer on each call. This module resolves that getter by signature and calls
 * it the same way, which is the same shape the camera pose block is reached by.
 *
 * No lane the Client authors is written down anywhere. A block that is not the one this module
 * last wrote is the Client's own, so reading before each hold both takes the value a lane is put
 * back to and follows an activity change, which re-authors the whole block.
 */

#include "inactivity_override.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../inactivity/inactivity_settings_store.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::inactivity {
namespace {

namespace settings = client::inactivity;

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * The activity config getter. Its body is the shared shape every obfuscated pointer getter has,
 * so the load of its own global is what tells it apart: a RIP-relative displacement encodes a
 * distance rather than an address, carries no position-dependent bytes, and is the only part of
 * this prologue unique to this getter. Call and branch displacements stay wildcarded.
 */
constexpr std::string_view kConfigGetterText =
    "40 53 48 83 EC 20 48 8B 1D 2B 10 1A 02 48 85 DB 0F 84 ? ? ? ? 48 89 5C 24 30 "
    "E8 ? ? ? ? 33 C3";
/** Compiled pattern bytes of the config getter signature. */
constexpr auto kConfigGetter = signature<signature_length(kConfigGetterText)>(kConfigGetterText);

/**
 * The controlled player's index, which the idle clock is keyed by. Stopped at its own ret, because
 * the bytes after it belong to the next function and would tie this pattern to that one's layout.
 */
constexpr std::string_view kControlledIndexText =
    "48 8B 05 39 0F 26 02 8B 80 60 04 00 00 C3";
constexpr auto kControlledIndex =
    signature<signature_length(kControlledIndexText)>(kControlledIndexText);

/** The idle clock. It answers in the lanes' own unit, which is what makes the two comparable. */
constexpr std::string_view kIdleClockText =
    "40 53 48 83 EC 20 48 63 D9 48 8D 0D C8 99 A8 01 8B D3 E8 ? ? ? ? 84 C0";
constexpr auto kIdleClock = signature<signature_length(kIdleClockText)>(kIdleClockText);

/** The session clock, which is what the grace is measured against. */
constexpr std::string_view kSessionClockText =
    "48 83 EC 28 E8 ? ? ? ? 48 85 C0 74 ? 80 3D 4B 63 88 01 00 48 89 5C 24 20";
constexpr auto kSessionClock = signature<signature_length(kSessionClockText)>(kSessionClockText);

/**
 * The session grace, in the object the getter returns.
 *
 * Read so the interface can explain a lane that has not fired yet, and never written: this module
 * exists to stop a kick, and the only thing a shorter grace can do is bring one forward.
 */
constexpr std::size_t kGraceOffset = 0x84;

/** Where the lanes start in the object the getter returns. */
constexpr std::size_t kTimeoutBlockOffset = 0xAC;
/** Milliseconds between re-applications, so an activity change cannot outlast the hold. */
constexpr std::uint64_t kHoldIntervalMs = 2000;

/** Fourteen consecutive milliseconds, in block order. */
using Lanes = std::array<std::uint32_t, settings::kActivityCount>;
/** Bytes of the block. */
constexpr std::size_t kBlockBytes = sizeof(Lanes);

/** Returns the activity config object. The pointer in its global is obfuscated, so we call it. */
using ConfigGetter = std::byte*(__fastcall*)();
/** Answers -1 when nothing is being controlled, which is not an error and not an index. */
using IndexGetter = std::int32_t(__fastcall*)();
using IdleGetter = std::uint64_t(__fastcall*)(std::int32_t);
using SessionGetter = std::uint64_t(__fastcall*)();

SRWLOCK g_lock{SRWLOCK_INIT};
ConfigGetter g_getter{};
/** The object the last call returned, kept only so the interface can show it. */
std::uintptr_t g_object{};
std::uint64_t g_nextHoldTick{};
/** The block this module last wrote. Anything else in the object is the Client's own. */
Lanes g_applied{};
bool g_appliedValid{};
/** The Client's own lanes for the activity in play. */
Lanes g_captured{};
bool g_capturedValid{};
/** Set while a hold is in place, so releasing it writes the captured lanes exactly once. */
bool g_holding{};
/** The intent the last poll acted on, so a changed one does not wait for the hold interval. */
Lanes g_intentLanes{};
bool g_intentHolding{};
bool g_intentValid{};
Lanes g_live{};
bool g_liveValid{};
/** Never written, so unlike the lanes there is nothing to capture and put back. */
std::uint32_t g_liveGrace{};
bool g_liveGraceValid{};

/** Null is a normal state: a build that does not match still holds its lanes without them. */
IndexGetter g_indexGetter{};
IdleGetter g_idleGetter{};
SessionGetter g_sessionGetter{};

/**
 * Calls the getter without faulting. The body is obfuscated game code, and it runs before the
 * Client has published its global on an early frame.
 * @return The activity config object, or null.
 */
[[nodiscard]] std::byte* config_object() noexcept {
    if (g_getter == nullptr) {
        return nullptr;
    }
    __try {
        return g_getter();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

/**
 * Reads the block out of the object.
 * @param object Config object.
 * @param values Receives the lanes.
 * @return True when Windows copied all of them.
 */
[[nodiscard]] bool read_block(const std::byte* object, Lanes& values) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             object + kTimeoutBlockOffset,
                             values.data(),
                             kBlockBytes,
                             &read)
               != FALSE
           && read == kBlockBytes;
}

/**
 * @param object Config object.
 * @param value Receives the milliseconds.
 * @return True when Windows copied it.
 */
[[nodiscard]] bool read_grace(const std::byte* object, std::uint32_t& value) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(
               GetCurrentProcess(), object + kGraceOffset, &value, sizeof value, &read)
               != FALSE
           && read == sizeof value;
}

/**
 * Writes one run of milliseconds into the object.
 * @param object Config object.
 * @param values Lanes in block order.
 * @return True when Windows copied all of them.
 */
[[nodiscard]] bool write_block(std::byte* object, const Lanes& values) noexcept {
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(),
                              object + kTimeoutBlockOffset,
                              values.data(),
                              kBlockBytes,
                              &written)
               != FALSE
           && written == kBlockBytes;
}

/**
 * Takes the Client's own lanes, which are any lanes this module did not write.
 * @param current Block just read out of the object.
 */
void capture_locked(const Lanes& current) noexcept {
    // A zero lane is an object the Client has published but not authored yet.
    const bool authored =
        std::none_of(current.begin(), current.end(), [](std::uint32_t value) noexcept {
            return value == 0;
        });
    if (!authored || (g_appliedValid && current == g_applied)) {
        return;
    }
    g_captured = current;
    g_capturedValid = true;
}

/** @return True while either switch asks for a hold. */
[[nodiscard]] bool holds(const settings::Settings& configured) noexcept {
    return configured.enabled || configured.custom;
}

/**
 * @param configured Current configuration.
 * @return The lanes a hold puts in place.
 */
[[nodiscard]] Lanes held_lanes(const settings::Settings& configured) noexcept {
    // A hand-edited file can carry both switches, so the blanket hold wins as the safer one.
    const bool set = configured.custom && !configured.enabled;
    Lanes values = set ? configured.timeouts : settings::kDefaultTimeouts;
    // Orbit is held at its longest whatever the grid or the file carries, because a timeout that
    // fires there ends a session this Client cannot re-establish.
    values[settings::kOrbitLane] = settings::kMaximumTimeoutMs;
    return values;
}

/**
 * Writes the captured lanes back and ends the hold.
 * @return True when lanes were put back, false when there was no hold to end.
 */
[[nodiscard]] bool release_locked(std::byte* object) noexcept {
    if (!g_holding || !g_capturedValid || !write_block(object, g_captured)) {
        return false;
    }
    g_holding = false;
    g_appliedValid = false;
    return true;
}

/**
 * Caller holds the lock. Failure is not propagated, because these are reported and never acted on:
 * a build whose signatures have moved should still install and still hold its lanes.
 */
void resolve_clocks_locked() noexcept {
    std::byte* const index = scan_main_image_unique(kControlledIndex, "inactivity_controlled_index");
    std::byte* const idle = scan_main_image_unique(kIdleClock, "inactivity_idle_clock");
    std::byte* const session = scan_main_image_unique(kSessionClock, "inactivity_session_clock");
    if (index == nullptr || idle == nullptr || session == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=inactivity stage=clocks result=fail");
        return;
    }
    g_indexGetter = reinterpret_cast<IndexGetter>(index);
    g_idleGetter = reinterpret_cast<IdleGetter>(idle);
    g_sessionGetter = reinterpret_cast<SessionGetter>(session);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=inactivity stage=clocks result=ok");
}

} // namespace

/** Resolves the activity config getter. */
bool install() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_getter != nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    std::byte* const match = scan_main_image_unique(kConfigGetter, "inactivity_config_getter");
    if (match == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=inactivity stage=install result=fail reason=target");
        return false;
    }
    g_getter = reinterpret_cast<ConfigGetter>(match);
    // Scanned here rather than on first display, because the scan walks the whole image and the
    // interface asks for these from the render thread.
    resolve_clocks_locked();
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=inactivity stage=install result=ok");
    return true;
}

/** Puts the Client's own lanes back and drops the resolved getter. */
void uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (std::byte* const object = config_object(); object != nullptr) {
        // Nothing to report on the way out; the lanes are put back or there was no hold.
        (void)release_locked(object);
    }
    g_getter = nullptr;
    g_indexGetter = nullptr;
    g_idleGetter = nullptr;
    g_sessionGetter = nullptr;
    g_object = 0;
    g_nextHoldTick = 0;
    g_applied = Lanes{};
    g_appliedValid = false;
    g_captured = Lanes{};
    g_capturedValid = false;
    g_holding = false;
    g_intentLanes = Lanes{};
    g_intentHolding = false;
    g_intentValid = false;
    g_live = Lanes{};
    g_liveValid = false;
    g_liveGrace = 0;
    g_liveGraceValid = false;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Holds the configured milliseconds, or puts back the ones the Client authored. */
void poll() noexcept {
    const settings::Settings configured = settings::get();
    const bool holding = holds(configured);
    const Lanes desired = held_lanes(configured);
    AcquireSRWLockExclusive(&g_lock);
    const std::uint64_t now = GetTickCount64();
    // A changed intent is the operator waiting on this call, so it does not wait for the interval.
    const bool changed =
        !g_intentValid || g_intentHolding != holding || (holding && g_intentLanes != desired);
    if (g_getter == nullptr || (now < g_nextHoldTick && !changed)) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_nextHoldTick = now + kHoldIntervalMs;
    // Recorded before the object is reached, so neither a poll that finds no activity nor a write
    // the Client refuses can leave the intent looking changed and skip the interval on every later
    // frame.
    g_intentLanes = desired;
    g_intentHolding = holding;
    g_intentValid = true;
    std::byte* const object = config_object();
    g_object = reinterpret_cast<std::uintptr_t>(object);
    if (object == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (Lanes current{}; read_block(object, current)) {
        g_live = current;
        g_liveValid = true;
        capture_locked(current);
    }
    if (std::uint32_t grace = 0; read_grace(object, grace)) {
        g_liveGrace = grace;
        g_liveGraceValid = true;
    }
    if (!holding) {
        const bool released = release_locked(object);
        ReleaseSRWLockExclusive(&g_lock);
        // Logged as its own event, so a reader can see a hold end rather than only see one start.
        // Nothing to put back is not a failure: it is the ordinary state with the feature off.
        if (changed) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             released ? "ev=inactivity stage=release result=ok"
                                      : "ev=inactivity stage=release result=noop");
        }
        return;
    }
    // Held rather than written once, because an activity change re-authors these lanes.
    const bool wrote = write_block(object, desired);
    if (wrote) {
        g_applied = desired;
        g_appliedValid = true;
        g_holding = true;
    }
    // The Client picks which lane to time by at runtime, so the shortest one is the only figure
    // that says when a kick can first happen without naming a lane that may not be in force.
    const std::uint32_t shortest = *std::min_element(desired.begin(), desired.end());
    ReleaseSRWLockExclusive(&g_lock);
    if (changed) {
        // Only on a change, so a steady hold does not fill the log every interval.
        std::array<char, 128> line{};
        const int length = std::snprintf(line.data(),
                                         line.size(),
                                         "ev=inactivity stage=hold mode=%s shortest_ms=%u result=%s",
                                         configured.enabled ? "disable" : "set",
                                         shortest,
                                         wrote ? "ok" : "fail");
        if (length > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(length)});
        }
    }
}

/** Reports what the override reached. */
Status status() noexcept {
    Status output{};
    AcquireSRWLockShared(&g_lock);
    output.resolved = g_getter != nullptr;
    output.address = g_object;
    output.captured = g_capturedValid;
    output.live = g_live;
    output.liveValid = g_liveValid;
    output.liveGraceMs = g_liveGrace;
    output.liveGraceValid = g_liveGraceValid;
    ReleaseSRWLockShared(&g_lock);
    return output;
}

Timers timers() noexcept {
    Timers output{};
    AcquireSRWLockShared(&g_lock);
    const IndexGetter index = g_indexGetter;
    const IdleGetter idle = g_idleGetter;
    const SessionGetter session = g_sessionGetter;
    ReleaseSRWLockShared(&g_lock);
    if (index == nullptr || idle == nullptr || session == nullptr) {
        return output;
    }
    output.resolved = true;
    // Called outside the lock and guarded, because these bodies are obfuscated Client code and
    // run before the Client has published the globals they read on an early frame.
    __try {
        const std::int32_t controlled = index();
        if (controlled >= 0) {
            output.idleMs = idle(controlled);
            output.idleValid = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.idleValid = false;
    }
    __try {
        output.sessionMs = session();
        output.sessionValid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.sessionValid = false;
    }
    return output;
}

} // namespace sunrise::client::hooks::inactivity
