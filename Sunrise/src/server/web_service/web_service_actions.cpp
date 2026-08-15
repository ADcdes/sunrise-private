#include "web_service_actions.h"

#include <array>
#include <cstdio>
#include <limits>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/encoding/bit_reader.h"
#include "../../middleware/encoding/byte_order.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../state/account/account_state.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::web_service {

namespace {

constexpr std::size_t kEquipmentActionPayloadSize = middleware::encoding::kU64Size + 1U;
constexpr std::size_t kItemStatePayloadSize = 15;
constexpr std::uint8_t kItemStateDefinitionIndexWidth = 15;
constexpr std::uint8_t kItemStateValueWidth = 32;
constexpr std::uint8_t kItemStatePaddingWidth = 7;
constexpr std::uint64_t kItemStateValueBias = 0x80000000ULL;
constexpr std::size_t kItemDismantlePayloadSize = 16;
constexpr std::uint8_t kItemDismantleInstanceWidth = 64;
constexpr std::uint8_t kItemDismantleDefinitionIndexWidth = 15;
constexpr std::uint8_t kItemDismantleQuantityWidth = 32;
constexpr std::uint32_t kItemDismantleSingleQuantityWire = 0x80000001U;
constexpr std::uint8_t kItemDismantleRequiredFlagWidth = 1;
constexpr std::uint8_t kItemDismantleNestedPaddingWidth = 6;
constexpr std::uint8_t kItemDismantleOuterTrailerWidth = 2;
constexpr std::uint8_t kItemDismantleFinalPaddingWidth = 6;
constexpr std::uint64_t kEquipmentSelectorStride = 4;
constexpr std::uint8_t kEquippedShaderModelSocketKind = 0;
constexpr std::size_t kItemAcquisitionPayloadSize = 3;
constexpr std::uint8_t kItemAcquisitionPresenceWidth = 1;
constexpr std::uint8_t kItemAcquisitionCollectibleIndexWidth = 15;
constexpr std::uint8_t kItemAcquisitionPaddingWidth = 8;
constexpr std::uint32_t kUnavailableDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();
} // namespace

/** Logs one exact correlated equipment response after its Queuez update is staged. */
void report_equip_response(const middleware::web_service::Message& message,
                           std::int32_t family4Version,
                           std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=equipment stage=response opcode=%u transaction=%u "
                                     "family_version=%d bytes=%zu hex=",
                                     static_cast<unsigned>(message.opcode),
                                     static_cast<unsigned>(message.transactionId),
                                     family4Version,
                                     response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t length = static_cast<std::size_t>(prefix);
    for (const std::byte byte : response) {
        if (length + 2 >= line.size()) {
            break;
        }
        const unsigned value = std::to_integer<unsigned>(byte);
        line[length++] = kHex[(value >> 4U) & 0xFU];
        line[length++] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final item-creation status pair and the exact Family-4 revision it promises. */
void report_item_acquisition_response(const middleware::web_service::Message& message,
                                      std::int32_t family4Version,
                                      std::uint64_t acquiredInstanceSoid,
                                      std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=acquire stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(acquiredInstanceSoid),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t length = static_cast<std::size_t>(prefix);
    for (const std::byte byte : response) {
        if (length + 2 >= line.size()) {
            break;
        }
        const unsigned value = std::to_integer<unsigned>(byte);
        line[length++] = kHex[(value >> 4U) & 0xFU];
        line[length++] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final profile-stack status pair and the exact Family-4 account revision it promises. */
void report_profile_item_acquisition_response(const middleware::web_service::Message& message,
                                              std::int32_t family4Version,
                                              std::uint32_t definitionHash,
                                              std::int32_t quantity,
                                              std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=profile_acquire stage=response result=ok opcode=%u transaction=%u "
                      "family_version=%d definition_hash=%u quantity=%d bytes=%zu hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      family4Version,
                      definitionHash,
                      quantity,
                      response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t length = static_cast<std::size_t>(prefix);
    for (const std::byte byte : response) {
        if (length + 2 >= line.size()) {
            break;
        }
        const unsigned value = std::to_integer<unsigned>(byte);
        line[length++] = kHex[(value >> 4U) & 0xFU];
        line[length++] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final dismantle status pair and the exact Family-4 revision it promises. */
void report_item_dismantle_response(const middleware::web_service::Message& message,
                                    std::int32_t family4Version,
                                    std::uint64_t dismantledInstanceSoid,
                                    std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=dismantle stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(dismantledInstanceSoid),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t length = static_cast<std::size_t>(prefix);
    for (const std::byte byte : response) {
        if (length + 2 >= line.size()) {
            break;
        }
        const unsigned value = std::to_integer<unsigned>(byte);
        line[length++] = kHex[(value >> 4U) & 0xFU];
        line[length++] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the exact opcode-903 status pair and the item-instance revision it promises. */
void report_socket_plug_response(const middleware::web_service::Message& message,
                                 std::int32_t family4Version,
                                 std::uint64_t targetInstanceSoid,
                                 std::uint8_t socketLane,
                                 std::uint16_t plugDefinitionIndex,
                                 std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=socket_plug stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX lane=%u plug_definition=%u bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(targetInstanceSoid),
        static_cast<unsigned>(socketLane),
        static_cast<unsigned>(plugDefinitionIndex),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t length = static_cast<std::size_t>(prefix);
    for (const std::byte byte : response) {
        if (length + 2 >= line.size()) {
            break;
        }
        const unsigned value = std::to_integer<unsigned>(byte);
        line[length++] = kHex[(value >> 4U) & 0xFU];
        line[length++] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** One line carries the picked id and whether the selection moved. */
constexpr std::size_t kSelectLineCapacity = 96;

/**
 * Records the player's character pick, which arrives nowhere else.
 * A bad or unknown id leaves the selection alone. The reply is the status pair either way. The
 * Family-4 object move follows this call, and the family-zero pair after it.
 * @param message Parsed select-character request.
 * @param outcome Gets the picked key once the selection has moved in State.
 */
void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode504::Request picked;
    if (!middleware::web_service::messages::opcode504::parse_request(message, picked)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws504 stage=parse result=fail");
        return;
    }
    bool changed = false;
    if (!state::set_selected_character(picked.characterSoid, changed)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws504 stage=select result=unknown");
        return;
    }
    outcome.hasSelectedCharacter = true;
    outcome.selectedCharacterSoid = picked.characterSoid;

    std::array<char, kSelectLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=ws504 stage=select result=ok soid=0x%llX changed=%u",
                                      static_cast<unsigned long long>(picked.characterSoid),
                                      static_cast<unsigned>(changed));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Parses the exact shared opcode-403/404 SOID descriptor. */
[[nodiscard]] bool parse_equipment_instance(const middleware::web_service::Message& message,
                                            std::uint64_t& instanceSoid) noexcept {
    instanceSoid = 0;
    if (message.payload.size() != kEquipmentActionPayloadSize
        || message.payload[middleware::encoding::kU64Size] != std::byte{}) {
        return false;
    }
    instanceSoid = middleware::encoding::read_u64_be(
        std::span<const std::byte, middleware::encoding::kU64Size>{message.payload.data(),
                                                                   middleware::encoding::kU64Size});
    return instanceSoid != 0;
}

/** Prepares one opcode-403/404 equipment mutation without publishing State early. */
void mutate_equipment(const middleware::web_service::Message& message,
                      bool unequip,
                      Outcome& outcome) noexcept {
    std::uint64_t requestedInstanceSoid = 0;
    if (!parse_equipment_instance(message, requestedInstanceSoid)) {
        std::array<char, 112> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=equipment stage=parse result=fail opcode=%u "
                                        "payload_bytes=%zu",
                                        static_cast<unsigned>(message.opcode),
                                        message.payload.size());
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    state::PendingEquipmentSwap mutation;
    const bool prepared = unequip
                              ? state::prepare_equipment_unequip(requestedInstanceSoid, mutation)
                              : state::prepare_equipment_swap(requestedInstanceSoid, mutation);
    if (!prepared) {
        std::array<char, 144> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=equipment stage=prepare result=fail opcode=%u action=%s requested=0x%llX",
            static_cast<unsigned>(message.opcode),
            unequip ? "unequip" : "equip",
            static_cast<unsigned long long>(requestedInstanceSoid));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }
    outcome.mutation = mutation;

    std::array<char, 224> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=equipment stage=prepare result=ok opcode=%u action=%s character=0x%llX "
                      "previous=0x%llX requested=0x%llX native_slot=%u moved_items=%zu",
                      static_cast<unsigned>(message.opcode),
                      unequip ? "unequip" : "equip",
                      static_cast<unsigned long long>(mutation.characterSoid),
                      static_cast<unsigned long long>(mutation.previousInstanceSoid),
                      static_cast<unsigned long long>(mutation.requestedInstanceSoid),
                      static_cast<unsigned>(mutation.nativeEquipmentSlot),
                      mutation.movedItemCount);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one exact selected-character opcode-903 socket selection. */
void mutate_socket_plug(const middleware::web_service::Message& message,
                        Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode903::Request request{};
    if (!middleware::web_service::messages::opcode903::parse_request(message, request)
        || !request.hasInstance || request.instanceSoid == 0 || request.hasTargetDefinition
        || !request.hasPlugDefinition
        || request.socketIndex >= state::account::inventory::kPlugCapacity) {
        std::array<char, 192> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws903 stage=parse result=fail transaction=%u payload_bytes=%zu has_instance=%u "
            "instance=0x%llX has_target_definition=%u socket=%u has_plug_definition=%u",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned>(request.hasInstance),
            static_cast<unsigned long long>(request.instanceSoid),
            static_cast<unsigned>(request.hasTargetDefinition),
            request.socketIndex,
            static_cast<unsigned>(request.hasPlugDefinition));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    state::PendingSocketPlug mutation{};
    if (!state::prepare_socket_plug(request.instanceSoid,
                                    static_cast<std::uint8_t>(request.socketIndex),
                                    request.plugDefinitionIndex,
                                    mutation)) {
        std::array<char, 192> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws903 stage=prepare result=fail transaction=%u instance=0x%llX lane=%u "
            "plug_definition=%u",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.instanceSoid),
            request.socketIndex,
            static_cast<unsigned>(request.plugDefinitionIndex));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 240> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws903 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "target_definition=%u target_bucket=%u lane=%u plug_definition=%u plug_bucket=%u "
        "equipped=%u item_index=%zu",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(mutation.targetEquipped),
        mutation.itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one character-location opcode-1901 socket selection. */
void mutate_equipped_socket_plug(const middleware::web_service::Message& message,
                                 Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode1901::Request request{};
    if (!middleware::web_service::messages::opcode1901::parse_request(message, request)
        || request.canonicalSocketKind != request.socketIndex
        || request.modelSocketKind != kEquippedShaderModelSocketKind || request.auxiliary != 0
        || request.socketIndex >= state::account::inventory::kPlugCapacity
        || request.equipmentSelector == 0
        || request.equipmentSelector % kEquipmentSelectorStride != 0) {
        std::array<char, 256> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws1901 stage=parse result=fail transaction=%u payload_bytes=%zu "
            "plug_definition=%u canonical_kind=%u model_kind=%u socket=%u auxiliary=0x%llX "
            "equipment_selector=%llu",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned>(request.plugDefinitionIndex),
            static_cast<unsigned>(request.canonicalSocketKind),
            static_cast<unsigned>(request.modelSocketKind),
            request.socketIndex,
            static_cast<unsigned long long>(request.auxiliary),
            static_cast<unsigned long long>(request.equipmentSelector));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    const std::uint64_t identityToken = request.equipmentSelector / kEquipmentSelectorStride;
    state::PendingSocketPlug mutation{};
    if (!state::prepare_character_selector_socket_plug(
            request.equipmentSelector,
            static_cast<std::uint8_t>(request.socketIndex),
            request.plugDefinitionIndex,
            mutation)) {
        std::array<char, 224> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws1901 stage=prepare result=fail transaction=%u equipment_selector=%llu "
            "identity_token=%llu lane=%u plug_definition=%u canonical_kind=%u model_kind=%u "
            "auxiliary=0x%llX",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.equipmentSelector),
            static_cast<unsigned long long>(identityToken),
            request.socketIndex,
            static_cast<unsigned>(request.plugDefinitionIndex),
            static_cast<unsigned>(request.canonicalSocketKind),
            static_cast<unsigned>(request.modelSocketKind),
            static_cast<unsigned long long>(request.auxiliary));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 288> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1901 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "equipment_selector=%llu identity_token=%llu target_definition=%u target_bucket=%u "
        "lane=%u plug_definition=%u plug_bucket=%u canonical_kind=%u model_kind=%u "
        "auxiliary=0x%llX",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned long long>(request.equipmentSelector),
        static_cast<unsigned long long>(identityToken),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(request.canonicalSocketKind),
        static_cast<unsigned>(request.modelSocketKind),
        static_cast<unsigned long long>(request.auxiliary));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one complete accumulated item-state value from opcode 406. */
void mutate_item_state(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::encoding::bits::Reader reader(message.payload);
    std::uint64_t instancePresent = 0;
    std::uint64_t instanceSoid = 0;
    std::uint64_t definitionPresent = 0;
    std::uint64_t definitionIndex = 0;
    std::uint64_t encodedFlags = 0;
    std::uint64_t padding = 0;
    if (message.payload.size() != kItemStatePayloadSize || !reader.read(1, instancePresent)
        || !reader.read(64, instanceSoid) || !reader.read(1, definitionPresent)
        || !reader.read(kItemStateDefinitionIndexWidth, definitionIndex)
        || !reader.read(kItemStateValueWidth, encodedFlags)
        || !reader.read(kItemStatePaddingWidth, padding) || reader.remaining_bits() != 0
        || instancePresent == 0 || instanceSoid == 0 || definitionPresent == 0
        || definitionIndex > (std::numeric_limits<std::uint16_t>::max)()
        || encodedFlags < kItemStateValueBias || padding != 0
        || encodedFlags - kItemStateValueBias > 0x3U) {
        std::array<char, 224> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws406 stage=parse result=fail transaction=%u payload_bytes=%zu instance=0x%llX "
            "definition=%llu flags_wire=0x%llX padding=0x%llX",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned long long>(instanceSoid),
            static_cast<unsigned long long>(definitionIndex),
            static_cast<unsigned long long>(encodedFlags),
            static_cast<unsigned long long>(padding));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    const std::uint32_t flags = static_cast<std::uint32_t>(encodedFlags - kItemStateValueBias);
    state::PendingItemState mutation{};
    if (!state::prepare_item_state(
            instanceSoid, static_cast<std::uint16_t>(definitionIndex), flags, mutation)) {
        return;
    }
    outcome.mutation = mutation;
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws406 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "definition=%u flags_before=0x%X flags_after=0x%X equipped=%u item_index=%zu",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        mutation.beforeFlags,
        mutation.afterFlags,
        mutation.targetEquipped ? 1U : 0U,
        mutation.itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Records strict opcode-402 parsing, identity checks, and State preparation outcomes. */
void report_item_dismantle(const middleware::web_service::Message& message,
                           std::string_view result,
                           std::string_view reason,
                           std::uint64_t instanceSoid,
                           std::uint32_t definitionIndex,
                           std::uint32_t definitionHash,
                           std::uint32_t quantity) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws402 stage=prepare result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "instance=0x%llX definition_index=%u definition_hash=%u quantity=%u",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        static_cast<unsigned long long>(instanceSoid),
        definitionIndex,
        definitionHash,
        quantity);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == "ok" ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Prepares the exact fixed-width opcode-402 Character-inventory removal request. */
void dismantle_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    if (message.payload.size() != kItemDismantlePayloadSize) {
        report_item_dismantle(
            message, "fail", "payload_size", 0, kUnavailableDefinitionIndex, 0, 0);
        return;
    }

    middleware::encoding::bits::Reader reader(message.payload);
    std::uint64_t instancePresent = 0;
    std::uint64_t instanceSoid = 0;
    std::uint64_t definitionPresent = 0;
    std::uint64_t encodedDefinitionIndex = 0;
    std::uint64_t encodedQuantity = 0;
    std::uint64_t requiredFlag = 0;
    std::uint64_t nestedPadding = 0;
    std::uint64_t outerTrailers = 0;
    std::uint64_t finalPadding = 0;
    if (!reader.read(1, instancePresent) || !reader.read(kItemDismantleInstanceWidth, instanceSoid)
        || !reader.read(1, definitionPresent)
        || !reader.read(kItemDismantleDefinitionIndexWidth, encodedDefinitionIndex)
        || !reader.read(kItemDismantleQuantityWidth, encodedQuantity)
        || !reader.read(kItemDismantleRequiredFlagWidth, requiredFlag)
        || !reader.read(kItemDismantleNestedPaddingWidth, nestedPadding)
        || !reader.read(kItemDismantleOuterTrailerWidth, outerTrailers)
        || !reader.read(kItemDismantleFinalPaddingWidth, finalPadding)
        || reader.remaining_bits() != 0) {
        report_item_dismantle(
            message, "fail", "payload_bits", 0, kUnavailableDefinitionIndex, 0, 0);
        return;
    }
    const auto definitionIndex = static_cast<std::uint16_t>(encodedDefinitionIndex);
    const auto quantityWire = static_cast<std::uint32_t>(encodedQuantity);
    constexpr std::uint32_t kSingleQuantity = 1;
    if (instancePresent == 0 || definitionPresent == 0 || instanceSoid == 0) {
        report_item_dismantle(
            message, "fail", "required_field", instanceSoid, definitionIndex, 0, 0);
        return;
    }
    if (nestedPadding != 0 || outerTrailers != 0 || finalPadding != 0) {
        report_item_dismantle(
            message, "fail", "padding_or_trailer", instanceSoid, definitionIndex, 0, 0);
        return;
    }
    if (quantityWire != kItemDismantleSingleQuantityWire || requiredFlag != 1) {
        report_item_dismantle(
            message, "fail", "quantity_or_flag", instanceSoid, definitionIndex, 0, 0);
        return;
    }

    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(definitionIndex, definition)) {
        report_item_dismantle(
            message, "fail", "definition", instanceSoid, definitionIndex, 0, kSingleQuantity);
        return;
    }
    state::PendingItemDismantle mutation{};
    if (!state::prepare_item_dismantle(instanceSoid, mutation)) {
        report_item_dismantle(message,
                              "fail",
                              "state",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    if (mutation.dismantledItem.definitionHash != definition.definitionHash
        || mutation.dismantledItem.quantity != static_cast<std::int32_t>(kSingleQuantity)) {
        report_item_dismantle(message,
                              "fail",
                              "identity",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    outcome.mutation = mutation;
    report_item_dismantle(message,
                          "ok",
                          "ready",
                          instanceSoid,
                          definitionIndex,
                          definition.definitionHash,
                          kSingleQuantity);
}

/** Records strict opcode-1820 parsing, installed mapping, and State preparation outcomes. */
void report_item_acquisition(const middleware::web_service::Message& message,
                             std::string_view result,
                             std::string_view reason,
                             std::uint32_t collectibleIndex,
                             std::uint32_t itemDefinitionIndex,
                             std::uint32_t definitionHash,
                             std::uint64_t instanceSoid) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1820 stage=prepare result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "collectible_index=%u item_definition_index=%u definition_hash=%u instance=0x%llX",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        collectibleIndex,
        itemDefinitionIndex,
        definitionHash,
        static_cast<unsigned long long>(instanceSoid));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == "ok" ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Prepares the exact three-byte opcode-1820 Collections item request. */
void acquire_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    if (message.payload.size() != kItemAcquisitionPayloadSize) {
        report_item_acquisition(message,
                                "fail",
                                "payload_size",
                                kUnavailableDefinitionIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }

    middleware::encoding::bits::Reader reader(message.payload);
    std::uint64_t present = 0;
    std::uint64_t encodedCollectibleIndex = 0;
    std::uint64_t padding = 0;
    if (!reader.read(kItemAcquisitionPresenceWidth, present)
        || !reader.read(kItemAcquisitionCollectibleIndexWidth, encodedCollectibleIndex)
        || !reader.read(kItemAcquisitionPaddingWidth, padding) || reader.remaining_bits() != 0) {
        report_item_acquisition(message,
                                "fail",
                                "payload_bits",
                                kUnavailableDefinitionIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }
    const auto collectibleIndex = static_cast<std::uint16_t>(encodedCollectibleIndex);
    if (present == 0) {
        report_item_acquisition(message,
                                "fail",
                                "collectible_absent",
                                collectibleIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }
    if (padding != 0) {
        report_item_acquisition(
            message, "fail", "padding", collectibleIndex, kUnavailableDefinitionIndex, 0, 0);
        return;
    }

    std::uint16_t itemDefinitionIndex = 0;
    if (!state::build_data::find_collectible_item_definition_index(collectibleIndex,
                                                                   itemDefinitionIndex)) {
        report_item_acquisition(message,
                                "fail",
                                "collectible_definition",
                                collectibleIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }

    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, definition)) {
        report_item_acquisition(
            message, "fail", "item_definition", collectibleIndex, itemDefinitionIndex, 0, 0);
        return;
    }

    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (!state::build_data::find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionIndex != itemDefinitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || !state::build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)) {
        report_item_acquisition(message,
                                "fail",
                                "item_detail_or_bucket",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    namespace bucket_domain = state::build_data::inventory::buckets;
    namespace detail_domain = state::build_data::items::details;
    if (bucket.arraySelector == bucket_domain::ArraySelector::profile) {
        if (detail.instancedDefinitionState != detail_domain::InstancedDefinitionState::stackable) {
            report_item_acquisition(message,
                                    "fail",
                                    "profile_item_instanced",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        state::PendingProfileItemAcquisition mutation{};
        if (!state::prepare_profile_item_acquisition(
                collectibleIndex, definition.definitionHash, mutation)) {
            report_item_acquisition(message,
                                    "fail",
                                    "profile_state",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        outcome.mutation = mutation;
        report_item_acquisition(message,
                                "ok",
                                "profile_ready",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }
    if (bucket.arraySelector != bucket_domain::ArraySelector::character) {
        report_item_acquisition(message,
                                "fail",
                                "unsupported_inventory_array",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    state::PendingItemAcquisition mutation{};
    if (!state::prepare_item_acquisition(collectibleIndex, definition.definitionHash, mutation)) {
        report_item_acquisition(message,
                                "fail",
                                "state",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }
    outcome.mutation = mutation;
    report_item_acquisition(message,
                            "ok",
                            "ready",
                            collectibleIndex,
                            itemDefinitionIndex,
                            definition.definitionHash,
                            mutation.acquiredInstanceSoid);
}

} // namespace sunrise::server::web_service
