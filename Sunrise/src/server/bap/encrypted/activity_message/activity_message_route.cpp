#include "activity_message_route.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/bap/activity_message/activity_client_identity_parser.h"
#include "../../../../middleware/bap/activity_message/activity_client_keepalive_validator.h"
#include "../../../../middleware/bap/activity_message/activity_high_water_validator.h"
#include "../../../../middleware/bap/activity_message/activity_join_request_parser.h"
#include "../../../../middleware/bap/activity_message/activity_membership_acknowledgement_parser.h"
#include "../../../../middleware/bap/activity_message/activity_message_request_parser.h"
#include "../../../../middleware/bap/activity_message/activity_state_refresh_parser.h"
#include "../../../../middleware/bap/activity_message/client_authoritative_data.h"
#include "../../../../middleware/bap/activity_message/entity_slots.h"
#include "../../../../state/activity/runtime.h"
#include "membership/activity_membership_route.h"
#include "middleware/bap/activity_message/activity_entity_slot_request_parser.h"
#include "patch_epoch/activity_patch_epoch_route.h"

namespace sunrise::server::bap::encrypted::activity_message {
namespace {

namespace service = middleware::bap::activity_message;
namespace client_keepalive = service::client_keepalive;
namespace high_water = service::high_water;
namespace epoch_message = service::patch_epoch;

/** Activity message type 3 starts the client join transaction. */
constexpr std::uint32_t kJoinRequestMessageType = 3;

/**
 * Reports one activity message the route did not stage, naming its type.
 * Every inbound activity message is one-way, so nothing here can jam the Client's reply ring. An
 * unnamed drop is invisible, and membership waits on the identity message.
 * @param messageType Activity message type from the envelope.
 * @param accountHandle Handle the envelope carried.
 * @param reason Short name of the step that declined.
 */
void report_message(std::uint32_t messageType,
                    std::uint64_t accountHandle,
                    const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=message result=skip type=%u "
                                      "handle=0x%llX reason=%s",
                                      messageType,
                                      static_cast<unsigned long long>(accountHandle),
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Prepares the joined State and the whole initial lease mask as one mutation.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives join scalars and the chosen lease mask.
 * @return True when the fixed join payload and current State can stage together.
 */
[[nodiscard]] bool prepare_join(const service::Request& request, ActivityPlan& plan) noexcept {
    service::JoinRequest parsed;
    if (!service::join_request::parse_join_request(request.payload, parsed)
        || parsed.sessionId != request.accountHandle
        || !state::activity::entity_slots::prepare_join(parsed.sessionId,
                                                        parsed.memberKey,
                                                        state::activity::entity_slots::kSlotCount,
                                                        plan.entitySlotMutation)) {
        return false;
    }
    plan.correlation = parsed.correlation;
    plan.sessionId = parsed.sessionId;
    plan.joinCharacterSoid = parsed.characterSoid;
    plan.delivery = Delivery::joinNotifications;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

/**
 * Prepares only currently free slots for one positive client request.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the chosen lease mask.
 * @return True for a valid positive request, including an exhausted zero-mask grant.
 */
[[nodiscard]] bool prepare_grant(const service::Request& request, ActivityPlan& plan) noexcept {
    std::int32_t requested = 0;
    if (!service::entity_slot_request::parse_entity_slot_request(request.payload, requested)
        || requested <= 0
        || !state::activity::entity_slots::prepare_grant(
            request.accountHandle, static_cast<std::size_t>(requested), plan.entitySlotMutation)) {
        return false;
    }
    plan.sessionId = request.accountHandle;
    plan.delivery = Delivery::entitySlotNotification;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

/**
 * Prepares only the slots that are both held and in the returned mask.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the chosen release mask.
 * @return True when the exact mask decodes and its session can stage a release.
 */
[[nodiscard]] bool prepare_release(const service::Request& request, ActivityPlan& plan) noexcept {
    service::entity_slots::EntitySlotMask decoded{};
    if (!service::entity_slots::decode_entity_slots(request.payload, decoded)) {
        return false;
    }
    state::activity::entity_slots::LeaseMask returned{};
    std::copy(decoded.begin(), decoded.end(), returned.begin());
    if (!state::activity::entity_slots::prepare_release(
            request.accountHandle, returned, plan.entitySlotMutation)) {
        return false;
    }
    plan.sessionId = request.accountHandle;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

} // namespace

/** Routes one svc8 activity message and prepares any supported push transaction. */
bool process(std::uint64_t boundSessionId,
             std::span<const std::byte> requestBody,
             ActivityPlan& plan,
             bool& hasTransaction) noexcept {
    plan = {};
    hasTransaction = false;

    service::Request request;
    if (!service::parse_request(requestBody, request)) {
        report_message(0, 0, "parse");
        return false;
    }
    // Dispatch is on message type alone, and every handler keys off the envelope's own handle, so
    // nothing has to be bound first. The join request carries the session in the first place, and
    // it arrives on a link that has allocated nothing.
    bool prepared = false;
    if (request.messageType == epoch_message::kMessageType) {
        // Type 52 alone carries a zero handle, so its session is the one this link allocated.
        prepared = patch_epoch::prepare(boundSessionId, request, plan);
    } else if (request.messageType == high_water::kMessageType
               || request.messageType == client_keepalive::kMessageType) {
        // Both are one-way notices with nothing to answer.
        return true;
    } else if (request.messageType == kJoinRequestMessageType) {
        prepared = prepare_join(request, plan);
    } else if (request.messageType == service::entity_slot_request::kMessageType) {
        prepared = prepare_grant(request, plan);
    } else if (request.messageType == service::entity_slots::kRequestMessageType) {
        prepared = prepare_release(request, plan);
    } else if (request.messageType == service::state_refresh::kMessageType) {
        prepared = membership::prepare_refresh(request, plan);
    } else if (request.messageType == service::client_identity::kMessageType) {
        prepared = membership::prepare_identity(request, plan);
    } else if (request.messageType == service::client_authoritative_data::kMessageType) {
        prepared = membership::prepare_authoritative(request, plan);
    } else if (request.messageType == service::membership_acknowledgement::kMessageType) {
        prepared = membership::prepare_acknowledgement(request, plan);
    } else {
        // Later message handlers are independent. An owned envelope is a safe no-op.
        report_message(request.messageType, request.accountHandle, "unhandled");
        return true;
    }
    // A message that cannot be staged is reported and dropped. Failing the frame would leave the
    // Client's pending ring jammed.
    if (!prepared) {
        report_message(request.messageType, request.accountHandle, "prepare");
        plan = {};
        return true;
    }
    hasTransaction = true;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_message
