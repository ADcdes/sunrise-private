#include "../../../../core/logging/log.h"
#include "../../../../middleware/bap/account_translation/account_translation_response.h"
#include "../../../../middleware/bap/activity_host/activity_host_response.h"
#include "../../../../middleware/bap/certificate.h"
#include "../../../../middleware/bap/client_config/client_config_response.h"
#include "../../../../middleware/bap/family_subscription.h"
#include "../../../../middleware/bap/family_unsubscription.h"
#include "../../../../middleware/bap/user_message/user_message_response.h"
#include "../../../../middleware/web_service/messages/opcode505/opcode505_codec.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../web_service/web_service_runtime.h"
#include "../activity_host_manager/activity_host_manager_route.h"
#include "../activity_message/activity_message_route.h"
#include "../internal.h"
#include "../matchmaking/matchmaking_route.h"
#include "../queuez/queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::body {

/**
 * Processes the body for one authenticated service route.
 * @param route Service route data found earlier.
 * @param queuezState Queuez versions and residents set up by this BAP peer.
 * @param activitySessionId Activity capability allocated through this BAP session.
 * @param matchmakingContext State-owned logical context for this BAP session.
 * @param requestBody Borrowed decrypted request body.
 * @param output Caller-owned response-body storage.
 * @param written Receives encoded body bytes.
 * @param outcome Receives one validated transport action or deferred State transaction.
 * @return True when the chosen body codec succeeds.
 */
bool process(const ServiceRoute& route,
             const queuez::SessionState& queuezState,
             std::uint64_t activitySessionId,
             state::matchmaking::ContextHandle matchmakingContext,
             std::span<const std::byte> requestBody,
             std::span<std::byte> output,
             std::size_t& written,
             ServiceOutcome& outcome) noexcept {
    outcome = {};
    switch (route.bodyCodec) {
    case BodyCodec::empty:
        written = 0;
        return true;
    case BodyCodec::accountTranslationResponse: {
        const state::AccountState account = state::account_snapshot();
        return middleware::bap::account_translation::encode_response(
            requestBody, account.primarySoid, output, written);
    }
    case BodyCodec::activityHostManagerResponse:
        return activity_host_manager::encode_response(requestBody,
                                                      output,
                                                      written,
                                                      outcome.activitySessionAllocation,
                                                      outcome.hasActivitySessionAllocation);
    case BodyCodec::activityMessageRequest:
        written = 0;
        return activity_message::process(
            activitySessionId, requestBody, outcome.activityPlan, outcome.hasActivityTransaction);
    case BodyCodec::activityHostResponse: {
        const state::SignOnState& signOn = state::sign_on();
        return middleware::bap::activity_host::encode_response(
            requestBody, signOn.relayAddress, signOn.relayPort, output, written);
    }
    case BodyCodec::clientConfigResponse:
        return middleware::bap::client_config::encode_minimal_response(output, written);
    case BodyCodec::familySubscription:
        written = 0;
        outcome.hasSubscription =
            middleware::bap::family_subscription::parse(requestBody, outcome.subscription);
        return outcome.hasSubscription;
    case BodyCodec::familyUnsubscription: {
        written = 0;
        outcome.hasUnsubscription =
            middleware::bap::family_unsubscription::parse(requestBody, outcome.unsubscription);
        return outcome.hasUnsubscription;
    }
    case BodyCodec::matchmakingResponse:
        return matchmaking::encode_response(matchmakingContext,
                                            requestBody,
                                            output,
                                            written,
                                            outcome.matchmakingMutation,
                                            outcome.hasMatchmakingMutation);
    case BodyCodec::steamCertificate:
        return middleware::bap::certificate::encode_response(requestBody, output, written);
    case BodyCodec::userMessageResponse:
        return middleware::bap::user_message::encode_minimal_response(output, written);
    case BodyCodec::webService: {
        middleware::web_service::Message message;
        if (middleware::web_service::parse_request(requestBody, message)
            && message.opcode == middleware::web_service::messages::opcode505::kOpcode) {
            if (!middleware::web_service::messages::opcode505::parse_request(message)
                || !queuez::stage_change_character(queuezState, outcome.changeCharacter)
                || !middleware::web_service::messages::opcode505::encode_response(
                    message, outcome.changeCharacter.after.family4Version, output, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws505 stage=change result=fail");
                // The plain status pair still goes out. The Client's Change Character waits on the
                // echoed transaction id, so a missing reply hangs it for the rest of the run.
                outcome.changeCharacter = {};
                return middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    middleware::web_service::StatusResponse{},
                    output,
                    written);
            }
            outcome.hasChangeCharacter = true;
            return true;
        }
        web_service::Outcome webOutcome;
        if (!sunrise::server::web_service::consume(requestBody, output, written, webOutcome)) {
            return false;
        }
        outcome.hasSubscription = webOutcome.hasSubscription;
        outcome.subscription = webOutcome.subscription;
        if (webOutcome.hasEquipmentSwap) {
            // Equip is an optimistic Character-screen action. Its status-pair value is the exact
            // Family-4 revision whose following Queuez frame makes the action authoritative. Stage
            // that revision before encoding the reply so the Client cannot complete the action
            // against the old object store.
            if (!queuez::stage_equipment_swap(queuezState,
                                              webOutcome.equipmentSwap.characterSoid,
                                              outcome.equipmentSwapUpdate)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws403 stage=queuez_preflight result=fail");
                // Keep the already-encoded sentinel response and publish no mutation, matching
                // the change-character failure contract instead of dropping the correlated task.
                outcome.equipmentSwapUpdate = {};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = outcome.equipmentSwapUpdate.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=ws403 stage=response result=fail");
                    return false;
                }
                web_service::report_equip_response(message, status.value, output.first(written));
                outcome.hasEquipmentSwap = true;
                outcome.equipmentSwap = webOutcome.equipmentSwap;
            }
        }
        if (webOutcome.hasSocketPlug) {
            // Opcode 903 completes at the exact Family-4 revision carrying the changed resident
            // item instance. The resident manifest and character placement remain unchanged.
            if (!queuez::stage_socket_plug(queuezState,
                                           webOutcome.socketPlug.accountSoid,
                                           webOutcome.socketPlug.characterSoid,
                                           webOutcome.socketPlug.targetInstanceSoid,
                                           webOutcome.socketPlug.profileChanged,
                                           outcome.socketPlugUpdate)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=socket_plug stage=queuez_preflight result=fail");
                outcome.socketPlugUpdate = {};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = outcome.socketPlugUpdate.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=socket_plug stage=response result=fail");
                    return false;
                }
                web_service::report_socket_plug_response(message,
                                                         status.value,
                                                         webOutcome.socketPlug.targetInstanceSoid,
                                                         webOutcome.socketPlug.socketLane,
                                                         webOutcome.socketPlug.plugDefinitionIndex,
                                                         output.first(written));
                outcome.hasSocketPlug = true;
                outcome.socketPlug = webOutcome.socketPlug;
            }
        }
        if (webOutcome.hasItemState) {
            // Opcode 406 completes at the exact Family-4 revision carrying the changed inventory
            // row flags. Placement and every resident item-instance body remain unchanged.
            if (!queuez::stage_equipment_swap(
                    queuezState, webOutcome.itemState.characterSoid, outcome.itemStateUpdate)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=item_state stage=queuez_preflight result=fail");
                outcome.itemStateUpdate = {};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = outcome.itemStateUpdate.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=item_state stage=response result=fail");
                    return false;
                }
                outcome.hasItemState = true;
                outcome.itemState = webOutcome.itemState;
            }
        }
        if (webOutcome.hasItemAcquisition) {
            // A Collections pull is complete only at the exact Family-4 revision that adds both
            // the inventory row and its newly resident instance object. Stage that revision before
            // re-encoding the correlated status pair, just like an equipment swap.
            if (!queuez::stage_item_acquisition(queuezState,
                                                webOutcome.itemAcquisition.accountSoid,
                                                webOutcome.itemAcquisition.characterSoid,
                                                webOutcome.itemAcquisition.acquiredInstanceSoid,
                                                webOutcome.itemAcquisition.profileChanged,
                                                outcome.itemAcquisitionUpdate)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=acquire stage=queuez_preflight result=fail");
                outcome.itemAcquisitionUpdate = {};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = outcome.itemAcquisitionUpdate.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=acquire stage=response result=fail");
                    return false;
                }
                web_service::report_item_acquisition_response(
                    message,
                    status.value,
                    webOutcome.itemAcquisition.acquiredInstanceSoid,
                    output.first(written));
                outcome.hasItemAcquisition = true;
                outcome.itemAcquisition = webOutcome.itemAcquisition;
            }
        }
        if (webOutcome.hasProfileItemAcquisition) {
            // Profile stacks live in the account body. Actionable shaders/modifications also name
            // a Family-4 item resident: an existing stack must already own it, while a newly
            // appended row adds it atomically at this exact +1 revision.
            if (!queuez::stage_profile_item_acquisition(
                    queuezState,
                    webOutcome.profileItemAcquisition.accountSoid,
                    webOutcome.profileItemAcquisition.acquiredInstanceSoid,
                    webOutcome.profileItemAcquisition.actionSource,
                    webOutcome.profileItemAcquisition.appended,
                    outcome.profileItemAcquisitionUpdate)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=profile_acquire stage=queuez_preflight result=fail");
                outcome.profileItemAcquisitionUpdate = {};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = outcome.profileItemAcquisitionUpdate.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=profile_acquire stage=response result=fail");
                    return false;
                }
                web_service::report_profile_item_acquisition_response(
                    message,
                    status.value,
                    webOutcome.profileItemAcquisition.acquiredDefinitionHash,
                    webOutcome.profileItemAcquisition.acquiredQuantity,
                    output.first(written));
                outcome.hasProfileItemAcquisition = true;
                outcome.profileItemAcquisition = webOutcome.profileItemAcquisition;
            }
        }
        if (webOutcome.hasItemDismantle) {
            // Dismantle is another optimistic Character-screen action. Promise only the exact
            // Family-4 revision that carries both the character after-image and the empty
            // item-instance release descriptor; otherwise retain the generic sentinel reply and
            // publish no removal.
            if (!queuez::stage_item_dismantle(queuezState,
                                              webOutcome.itemDismantle.accountSoid,
                                              webOutcome.itemDismantle.characterSoid,
                                              webOutcome.itemDismantle.dismantledInstanceSoid,
                                              webOutcome.itemDismantle.profileChanged,
                                              outcome.itemDismantleUpdate)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=dismantle stage=queuez_preflight result=fail");
                outcome.itemDismantleUpdate = {};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = outcome.itemDismantleUpdate.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=dismantle stage=response result=fail");
                    return false;
                }
                web_service::report_item_dismantle_response(
                    message,
                    status.value,
                    webOutcome.itemDismantle.dismantledInstanceSoid,
                    output.first(written));
                outcome.hasItemDismantle = true;
                outcome.itemDismantle = webOutcome.itemDismantle;
            }
        }
        // A pick that names the resident character moves nothing, so staging refuses it and the
        // reply still stands on its own.
        if (webOutcome.hasSelectedCharacter
            && queuez::stage_select_character(
                queuezState, webOutcome.selectedCharacterSoid, outcome.selectCharacter)) {
            outcome.hasSelectCharacter = true;
        } else {
            outcome.selectCharacter = {};
        }
        return true;
    }
    }
    written = 0;
    return false;
}

} // namespace sunrise::server::bap::encrypted::body
