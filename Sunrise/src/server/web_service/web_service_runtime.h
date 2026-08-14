#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/web_service/messages/opcode206.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::web_service {

/** Optional Server action produced while answering one Web Service request. */
struct Outcome {
    bool hasSubscription{};
    middleware::queuez::Subscription subscription{};
    /** An opcode-504 pick moved the selection and its Family-4 object still has to follow. */
    bool hasSelectedCharacter{};
    std::uint64_t selectedCharacterSoid{};
    /** Opcode 403/404 prepared one checked equipment mutation for the BAP transaction. */
    bool hasEquipmentSwap{};
    state::PendingEquipmentSwap equipmentSwap{};
    /** An item-creation route prepared one checked selected-character inventory insertion. */
    bool hasItemAcquisition{};
    state::PendingItemAcquisition itemAcquisition{};
    /** Opcode 1820 prepared one account-profile stack increment or insertion. */
    bool hasProfileItemAcquisition{};
    state::PendingProfileItemAcquisition profileItemAcquisition{};
    /** Opcode 402 prepared one checked selected-character inventory removal. */
    bool hasItemDismantle{};
    state::PendingItemDismantle itemDismantle{};
    /** Opcode 903 or 1901 prepared one exact selected-character ordinary-socket selection. */
    bool hasSocketPlug{};
    state::PendingSocketPlug socketPlug{};
    /** Opcode 406 prepared one complete accumulated item-state value. */
    bool hasItemState{};
    state::PendingItemState itemState{};
};

/** Records the final opcode-403/404 reply after its paired Family-4 version is known. */
void report_equip_response(const middleware::web_service::Message& message,
                           std::int32_t family4Version,
                           std::span<const std::byte> response) noexcept;

/** Records an item-creation reply after its exact Family-4 version and instance are known. */
void report_item_acquisition_response(const middleware::web_service::Message& message,
                                      std::int32_t family4Version,
                                      std::uint64_t acquiredInstanceSoid,
                                      std::span<const std::byte> response) noexcept;

/** Records the final profile-stack creation reply and exact Family-4 account revision. */
void report_profile_item_acquisition_response(const middleware::web_service::Message& message,
                                              std::int32_t family4Version,
                                              std::uint32_t definitionHash,
                                              std::int32_t quantity,
                                              std::span<const std::byte> response) noexcept;

/** Records a dismantle reply after its exact Family-4 version and removed instance are known. */
void report_item_dismantle_response(const middleware::web_service::Message& message,
                                    std::int32_t family4Version,
                                    std::uint64_t dismantledInstanceSoid,
                                    std::span<const std::byte> response) noexcept;

/** Records a socket-action reply after its exact item-instance Family-4 revision is known. */
void report_socket_plug_response(const middleware::web_service::Message& message,
                                 std::int32_t family4Version,
                                 std::uint64_t targetInstanceSoid,
                                 std::uint8_t socketLane,
                                 std::uint16_t plugDefinitionIndex,
                                 std::span<const std::byte> response) noexcept;

/**
 * Answers one whole supported Web Service request body.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @return False only when the envelope header does not parse.
 */
[[nodiscard]] bool consume(std::span<const std::byte> request,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

/**
 * Answers one request and reports any subscription side effect.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets a valid family selector only after the response is encoded.
 * @return False only when the envelope header does not parse.
 */
[[nodiscard]] bool consume(std::span<const std::byte> request,
                           std::span<std::byte> response,
                           std::size_t& written,
                           Outcome& outcome) noexcept;

} // namespace sunrise::server::web_service
