#include "web_service_runtime.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/encoding/bit_reader.h"
#include "../../middleware/encoding/byte_order.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode205.h"
#include "../../middleware/web_service/messages/opcode206.h"
#include "../../middleware/web_service/messages/opcode501_codec.h"
#include "../../middleware/web_service/messages/opcode503.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode601/opcode601_codec.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../middleware/web_service/web_service_envelope.h"
#include "../../state/account/account_state.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"
#include "opcode_routes.h"

namespace sunrise::server::web_service {

/** One ordinary event line carries an opcode and its fixed prefix. */
constexpr std::size_t kOpcodeLineCapacity = 64;
/** A request trace keeps enough payload to identify an item-action descriptor. */
constexpr std::size_t kRequestPayloadTraceBytes = 192;
/** Web Service opcode used by the Character screen's Equip action. */
constexpr std::uint16_t kEquipOpcode = 403;
/** Web Service opcode used by the Character screen's Unequip action. */
constexpr std::uint16_t kUnequipOpcode = 404;
/** Web Service opcode used by item-state actions such as finisher Favorite. */
constexpr std::uint16_t kItemStateOpcode = 406;
/** Opcode 406 carries an item identity, one biased 32-bit state value, and seven zero bits. */
constexpr std::size_t kItemStatePayloadSize = 15;
constexpr std::uint8_t kItemStateDefinitionIndexWidth = 15;
constexpr std::uint8_t kItemStateValueWidth = 32;
constexpr std::uint8_t kItemStatePaddingWidth = 7;
constexpr std::uint64_t kItemStateValueBias = 0x80000000ULL;
/** Opcodes 403 and 404 carry one SOID followed by one zero envelope trailer byte. */
constexpr std::size_t kEquipmentActionPayloadSize = middleware::encoding::kU64Size + 1U;
/** Web Service opcode used by the Character screen's Dismantle action. */
constexpr std::uint16_t kItemDismantleOpcode = 402;
/** Opcode 402 occupies one fixed 128-bit request descriptor. */
constexpr std::size_t kItemDismantlePayloadSize = 16;
/** The dismantled item instance is an optional 64-bit descriptor field. */
constexpr std::uint8_t kItemDismantleInstanceWidth = 64;
/** Installed item-definition rows fit the opcode's optional 15-bit descriptor field. */
constexpr std::uint8_t kItemDismantleDefinitionIndexWidth = 15;
/** Dismantle quantity is one signed 32-bit descriptor biased from INT32_MIN. */
constexpr std::uint8_t kItemDismantleQuantityWidth = 32;
/** Logical quantity one after applying the signed descriptor's minimum-int bias. */
constexpr std::uint32_t kItemDismantleSingleQuantityWire = 0x80000001U;
/** One required true flag follows the quantity in an instanced-item dismantle request. */
constexpr std::uint8_t kItemDismantleRequiredFlagWidth = 1;
/** Nested item-action alignment before the two absent outer-envelope trailers. */
constexpr std::uint8_t kItemDismantleNestedPaddingWidth = 6;
/** Two absent optional outer fields terminate the item-action request. */
constexpr std::uint8_t kItemDismantleOuterTrailerWidth = 2;
/** The fixed 128-bit request storage ends with six zero alignment bits. */
constexpr std::uint8_t kItemDismantleFinalPaddingWidth = 6;
/** Web Service opcode used by Collections to create one item instance. */
constexpr std::uint16_t kItemAcquisitionOpcode = 1820;
/** Opcode 1901 numbers flat equipment then native-inventory locations in one-based groups of 4. */
constexpr std::uint64_t kEquipmentSelectorStride = 4;
/** Captured equipped shader requests use the ordinary instanced-item socket descriptor arm. */
constexpr std::uint8_t kEquippedShaderModelSocketKind = 0;
/** Opcode 1820 carries one presence bit, a 15-bit collectible index, and one padding byte. */
constexpr std::size_t kItemAcquisitionPayloadSize = 3;
/** The optional collectible-index field starts with one presence bit. */
constexpr std::uint8_t kItemAcquisitionPresenceWidth = 1;
/** The installed collectible table is addressed by a 15-bit native index. */
constexpr std::uint8_t kItemAcquisitionCollectibleIndexWidth = 15;
/** One zero byte pads the request after its meaningful 16 bits. */
constexpr std::uint8_t kItemAcquisitionPaddingWidth = 8;
/** Failed parsing and mapping report the ordinary absent native-index sentinel. */
constexpr std::uint32_t kUnavailableDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();

/**
 * Logs which Web Service opcode arrived and a bounded payload trace. One svc-10 frame looks like
 *
 * any other in the log, and the opcodes and bit descriptors the Client sends drive its queuez
 *
 * state machine. The bounded hex is diagnostic protocol evidence, not a second parser.
 * @param
 * message Parsed request envelope and borrowed payload.
 */
void report_request(const middleware::web_service::Message& message) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=ws stage=request opcode=%u transaction=%u payload_bytes=%zu payload_hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      message.payload.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }

    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t length = static_cast<std::size_t>(prefix);
    const std::size_t traced = message.payload.size() < kRequestPayloadTraceBytes
                                   ? message.payload.size()
                                   : kRequestPayloadTraceBytes;
    for (std::size_t index = 0; index < traced && length + 2 < line.size(); ++index) {
        const unsigned value = std::to_integer<unsigned>(message.payload[index]);
        line[length++] = kHex[(value >> 4U) & 0xFU];
        line[length++] = kHex[value & 0xFU];
    }
    if (traced != message.payload.size()) {
        constexpr std::string_view kTruncated = " truncated=1";
        if (length + kTruncated.size() < line.size()) {
            std::memcpy(line.data() + length, kTruncated.data(), kTruncated.size());
            length += kTruncated.size();
        }
    }
    if (length != 0) {
        core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), length});
    }
}

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
    outcome.hasEquipmentSwap = true;
    outcome.equipmentSwap = mutation;

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

    outcome.hasSocketPlug = true;
    outcome.socketPlug = mutation;
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

    outcome.hasSocketPlug = true;
    outcome.socketPlug = mutation;
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
    outcome.hasItemState = true;
    outcome.itemState = mutation;
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
    outcome.hasItemDismantle = true;
    outcome.itemDismantle = mutation;
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
        outcome.hasProfileItemAcquisition = true;
        outcome.profileItemAcquisition = mutation;
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
    outcome.hasItemAcquisition = true;
    outcome.itemAcquisition = mutation;
    report_item_acquisition(message,
                            "ok",
                            "ready",
                            collectibleIndex,
                            itemDefinitionIndex,
                            definition.definitionHash,
                            mutation.acquiredInstanceSoid);
}

/**
 * Answers a request whose own codec refused with the bare correlated echo.
 * The Client matches on the echoed transaction id. A missing body is worse than a thin one. It
 * under-runs the decoder and takes the BAP connection down.
 * @param message Parsed request whose correlation fields are echoed.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size in bytes.
 * @return True when the echo fits.
 */
bool encode_echo(const middleware::web_service::Message& message,
                 std::span<std::byte> response,
                 std::size_t& written) noexcept {
    std::array<char, kOpcodeLineCapacity> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=ws stage=body result=echo opcode=%u", message.opcode);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    namespace ws = middleware::web_service;
    return ws::encode_response(
        message, ws::ResponseShape::generic, ws::StatusResponse{}, response, written);
}

/**
 * Parses and answers one Web Service request with its whole descriptor layout.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @return False only when the envelope header does not parse.
 */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    Outcome outcome;
    return consume(request, response, written, outcome);
}

/**
 * Parses one request, encodes its response, and publishes checked side effects last.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets a valid family selector only after the response is encoded.
 * @return False only when the envelope header does not parse.
 */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written,
             Outcome& outcome) noexcept {
    written = 0;
    outcome = {};
    middleware::web_service::Message message;
    if (!middleware::web_service::parse_request(request, message)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws stage=parse result=fail");
        return false;
    }
    report_request(message);

    if (message.opcode == middleware::web_service::messages::opcode205::kOpcode) {
        const auto investment = state::investment_snapshot();
        return middleware::web_service::messages::opcode205::encode_response(
                   message, investment, response, written)
               || encode_echo(message, response, written);
    }

    if (message.opcode == middleware::web_service::messages::opcode503::kOpcode) {
        middleware::web_service::messages::opcode503::Request bootstrap;
        const bool parsed =
            middleware::web_service::messages::opcode503::parse_request(message, bootstrap);
        // The request's own key is echoed and adopted. An authored id here costs the ship and the
        // banner.
        if (!bootstrap.hasPrimarySoid) {
            bootstrap.primarySoid = state::account_snapshot().primarySoid;
        }
        const auto investment = state::investment_snapshot();
        if (!parsed
            || !middleware::web_service::messages::opcode503::encode_response(
                message, bootstrap, investment, response, written)) {
            return encode_echo(message, response, written);
        }
        if (bootstrap.hasPrimarySoid && !state::set_primary_soid(bootstrap.primarySoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=ws503 stage=adopt result=fail");
        }
        return true;
    }

    if (message.opcode == middleware::web_service::messages::opcode501::kOpcode) {
        // Returns a SOID family three already publishes. The request body is not parsed.
        const std::uint64_t characterSoid =
            state::account::selected_character_soid(state::account_snapshot());
        return middleware::web_service::messages::opcode501::encode_response(
                   message, characterSoid, response, written)
               || encode_echo(message, response, written);
    }

    if (message.opcode == middleware::web_service::messages::opcode601::kOpcode) {
        return middleware::web_service::messages::opcode601::encode_response(
                   message, response, written)
               || encode_echo(message, response, written);
    }

    // A subscribe whose body does not parse is still answered; only the subscription is dropped.
    middleware::queuez::Subscription subscription;
    const bool subscribes =
        message.opcode == middleware::web_service::messages::opcode206::kOpcode
        && middleware::web_service::messages::opcode206::parse_request(message, subscription);

    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    if (!middleware::web_service::encode_response(
            message, shape, middleware::web_service::StatusResponse{}, response, written)) {
        return encode_echo(message, response, written);
    }
    if (subscribes) {
        // Publish the subscription only after its correlated response is complete.
        outcome.hasSubscription = true;
        outcome.subscription = subscription;
        return true;
    }
    if (message.opcode == middleware::web_service::messages::opcode504::kOpcode) {
        // The selection is State, not a response field, so it publishes after the reply encodes.
        select_character(message, outcome);
    } else if (message.opcode == kItemDismantleOpcode) {
        dismantle_item(message, outcome);
    } else if (message.opcode == kEquipOpcode) {
        mutate_equipment(message, false, outcome);
    } else if (message.opcode == kUnequipOpcode) {
        mutate_equipment(message, true, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode903::kOpcode) {
        mutate_socket_plug(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode1901::kOpcode) {
        mutate_equipped_socket_plug(message, outcome);
    } else if (message.opcode == kItemStateOpcode) {
        mutate_item_state(message, outcome);
    } else if (message.opcode == kItemAcquisitionOpcode) {
        acquire_item(message, outcome);
    }
    return true;
}

} // namespace sunrise::server::web_service
