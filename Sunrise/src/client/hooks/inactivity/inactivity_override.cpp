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

/**
 * @param configured Current configuration.
 * @return The lanes a hold puts in place.
 */
[[nodiscard]] Lanes held_lanes(const settings::Settings& configured) noexcept {
    Lanes values = configured.custom ? configured.timeouts : settings::kDefaultTimeouts;
    // Orbit is held at its longest whatever the grid or the file carries, because a timeout that
    // fires there ends a session this Client cannot re-establish.
    values[settings::kOrbitLane] = settings::kMaximumTimeoutMs;
    return values;
}

/** Writes the captured lanes back and ends the hold. */
void release_locked(std::byte* object) noexcept {
    if (!g_holding || !g_capturedValid || !write_block(object, g_captured)) {
        return;
    }
    g_holding = false;
    g_appliedValid = false;
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
        release_locked(object);
    }
    g_getter = nullptr;
    g_object = 0;
    g_nextHoldTick = 0;
    g_applied = Lanes{};
    g_appliedValid = false;
    g_captured = Lanes{};
    g_capturedValid = false;
    g_holding = false;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Holds the configured milliseconds, or puts back the ones the Client authored. */
void poll() noexcept {
    const settings::Settings configured = settings::get();
    AcquireSRWLockExclusive(&g_lock);
    const std::uint64_t now = GetTickCount64();
    if (g_getter == nullptr || now < g_nextHoldTick) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_nextHoldTick = now + kHoldIntervalMs;
    std::byte* const object = config_object();
    g_object = reinterpret_cast<std::uintptr_t>(object);
    if (object == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (Lanes current{}; read_block(object, current)) {
        capture_locked(current);
    }
    if (!configured.enabled) {
        release_locked(object);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    // Held rather than written once, because an activity change re-authors these lanes.
    const Lanes desired = held_lanes(configured);
    if (write_block(object, desired)) {
        g_applied = desired;
        g_appliedValid = true;
        g_holding = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Reports what the override reached. */
Status status() noexcept {
    Status output{};
    AcquireSRWLockShared(&g_lock);
    output.resolved = g_getter != nullptr;
    output.address = g_object;
    output.captured = g_capturedValid;
    ReleaseSRWLockShared(&g_lock);
    return output;
}

} // namespace sunrise::client::hooks::inactivity
