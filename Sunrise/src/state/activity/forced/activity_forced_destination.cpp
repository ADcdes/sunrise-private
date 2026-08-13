#include "activity_forced_destination.h"

#include <Windows.h>

#include <algorithm>

#include "../../runtime/storage/internal.h"

namespace sunrise::state::activity::forced {
namespace {

/** Bits in one byte, the step size of the descriptor's storage. */
constexpr std::size_t kBitsPerByte = 8;
/** The top bit of a byte, where every packed field starts. */
constexpr unsigned kHighBit = 0x80;
/** Every package-name element is one biased byte. */
constexpr std::uint8_t kNameElementBias = 0x80;

/**
 * Rewrites the package name inside one captured descriptor.
 * The field is a fixed 40 elements, so the descriptor keeps its length and every field
 * around the name keeps its bits.
 * @param selection Destination holding the captured descriptor.
 * @param name Forced package name.
 * @param length Bytes of that name.
 * @return True when the name field is inside the captured bits and was rewritten.
 */
[[nodiscard]] bool
rewrite_descriptor_name(destination::DestinationSelection& selection,
                        const std::array<char, destination::kPackageNameCapacity>& name,
                        std::size_t length) noexcept {
    const std::size_t fieldBits = destination::kPackageNameCapacity * kBitsPerByte;
    if (!selection.hasDescriptorName || selection.descriptorBitLength == 0
        || selection.descriptorNameBit + fieldBits > selection.descriptorBitLength) {
        return false;
    }
    for (std::size_t element = 0; element < destination::kPackageNameCapacity; ++element) {
        // The field is fixed width, so every element past the name is written as a biased zero.
        const auto character =
            element < length ? static_cast<std::uint8_t>(name[element]) : std::uint8_t{};
        const auto encoded = static_cast<unsigned>(character + kNameElementBias);
        for (std::size_t bit = 0; bit < kBitsPerByte; ++bit) {
            const std::size_t at = selection.descriptorNameBit + (element * kBitsPerByte) + bit;
            std::byte& target = selection.descriptorBits[at / kBitsPerByte];
            const unsigned mask = kHighBit >> (at % kBitsPerByte);
            const bool set = (encoded >> (kBitsPerByte - 1 - bit) & 1U) != 0;
            target = static_cast<std::byte>(set ? static_cast<unsigned>(target) | mask
                                                : static_cast<unsigned>(target) & ~mask);
        }
    }
    return true;
}

} // namespace

/** Replaces the forced destination. */
bool publish(const ForcedDestination& value) noexcept {
    if (!storable(value)) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state.activity.forced = value;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

/** Copies the forced destination. */
void snapshot(ForcedDestination& value) noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    value = runtime::storage::g_state.activity.forced;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
}

/** Drops the selection and the switch, the same as the interface's clear action. */
void clear() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state.activity.forced = {};
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Overwrites one committed destination with the forced one. */
bool apply(destination::DestinationSelection& selection) noexcept {
    ForcedDestination value{};
    snapshot(value);
    if (!active(value)) {
        return false;
    }

    selection.packageName = {};
    for (std::size_t index = 0; index < value.packageNameLength; ++index) {
        selection.packageName[index] = static_cast<std::int8_t>(value.packageName[index]);
    }
    selection.packageNameLength = value.packageNameLength;
    // All three are absent when a destination is forced rather than picked. Many package names
    // map to several definitions, so no index can be derived from a name.
    selection.reason = destination::kMinimumReason;
    selection.previousActivityIndex = destination::kAbsentActivityIndex;
    selection.activityIndex = destination::kAbsentActivityIndex;
    selection.elementIndex = destination::kAbsentElementIndex;
    selection.hasElementIndex = false;
    // The client named its arrival for the destination it picked, so both wire hashes go with it.
    selection.arrivalBubbleHash = 0;
    selection.hasArrivalBubbleHash = false;
    selection.spawnSetHash = 0;
    selection.hasSpawnSetHash = false;
    selection.arrivalBubbleOverride = value.bubble;
    selection.hasArrivalBubbleOverride = true;
    selection.sliceSetOverride = value.sliceSet;
    selection.hasSliceSetOverride = true;
    // With no set chosen the destination's own fallback stands: `default` where the map has one,
    // and the absent hash where it does not, which leaves the Client its loaded-world search.
    selection.spawnSetOverride = value.hasSpawnSetHash ? value.spawnSetHash : value.spawnFallback;
    selection.hasSpawnSetOverride = true;
    // Only the name inside the descriptor is rewritten. Re-encoding a small one drops the fields
    // with no known name, which leaves the Client's waiting overlay on screen.
    if (!rewrite_descriptor_name(selection, value.packageName, value.packageNameLength)) {
        selection.descriptorBits = {};
        selection.descriptorBitLength = 0;
        selection.descriptorNameBit = 0;
        selection.hasDescriptorName = false;
    }
    return true;
}

} // namespace sunrise::state::activity::forced
