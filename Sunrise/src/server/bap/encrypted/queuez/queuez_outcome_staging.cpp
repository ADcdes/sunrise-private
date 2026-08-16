#include "queuez_outcome_staging.h"

#include "../../../../core/logging/log.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::queuez {

/** Stages queuez subscription, unsubscription, or character-move output for one peer. */
bool stage_service_outcome(Scratch& scratch,
                           const SessionState& before,
                           const ServiceOutcome& outcome,
                           std::span<const std::byte, state::kAesKeySize> key,
                           std::array<std::byte, state::kBapNonceSize>& nonce,
                           std::span<std::byte> response,
                           std::size_t& written,
                           StagedPublication& publication) noexcept {
    publication = {};
    SessionState after{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    std::uint64_t bannerRoot = 0;
    if (outcome.hasSubscription) {
        push::append_queuez_notification(scratch,
                                         before,
                                         outcome.subscription,
                                         key,
                                         nonce,
                                         response,
                                         written,
                                         after,
                                         armsRepush,
                                         armsBannerRepush);
        bannerRoot = outcome.subscription.familyRootSoid;
    } else if (outcome.hasUnsubscription) {
        stage_unsubscription(before, outcome.unsubscription.familyRootSoid, after);
    } else if (outcome.hasChangeCharacter) {
        // The reply already carries the version this patch promises. A patch that cannot be built
        // leaves the ladder where it is, instead of holding back that reply.
        if (!push::append_change_character_notification(
                scratch, outcome.changeCharacter, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=change result=fail");
            return true;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = outcome.changeCharacter.after;
    } else if (outcome.hasSelectCharacter) {
        // The reply is the Client's task completion and the move is a separate frame. A move that
        // cannot be built leaves the selection where it is, instead of holding back that reply.
        if (!push::append_select_character_notification(
                scratch, outcome.selectCharacter, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=select result=fail");
            return true;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = outcome.selectCharacter.after;
        // The banner pair follows the family-four move, so it is sent against the state that move
        // made. A pair that cannot be built leaves the emblem where it is.
        const SessionState& bannerBefore = after;
        SessionState bannerAfter{};
        if (push::append_banner_move_notification(scratch,
                                                  bannerBefore,
                                                  outcome.selectCharacter.selectedCharacterSoid,
                                                  key,
                                                  nonce,
                                                  response,
                                                  written,
                                                  bannerAfter)) {
            after = bannerAfter;
        }
        // A family-zero subscribe that arrived before this pick was held, not answered. The pick
        // is the first moment the pair can be built, and the peer will not ask again.
        if (after.pendingBannerRoot != 0) {
            middleware::queuez::Subscription held{};
            held.familyType = kBannerFamilyType;
            held.familyRootSoid = after.pendingBannerRoot;
            SessionState heldAfter{};
            bool heldRepush = false;
            bool heldBannerRepush = false;
            const SessionState heldBefore = after;
            push::append_queuez_notification(scratch,
                                             heldBefore,
                                             held,
                                             key,
                                             nonce,
                                             response,
                                             written,
                                             heldAfter,
                                             heldRepush,
                                             heldBannerRepush);
            if (valid(heldAfter)) {
                after = heldAfter;
            }
            if (heldBannerRepush) {
                armsBannerRepush = true;
                bannerRoot = held.familyRootSoid;
            }
        }
    } else {
        return true;
    }
    // The frame is already written by here. A mirror that fails validation is logged and dropped,
    // never turned into a refusal to send what the Client waits for.
    publication.hasState = valid(after);
    if (publication.hasState) {
        publication.after = after;
    } else {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=publish result=unrecorded");
    }
    publication.armsFamily4Repush = armsRepush;
    publication.family4RepushRoot = armsRepush ? outcome.subscription.familyRootSoid : 0;
    publication.armsBannerRepush = armsBannerRepush && bannerRoot != 0;
    publication.bannerRepushRoot = publication.armsBannerRepush ? bannerRoot : 0;
    return true;
}

} // namespace sunrise::server::bap::encrypted::queuez
