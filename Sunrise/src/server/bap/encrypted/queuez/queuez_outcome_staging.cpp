#include "queuez_outcome_staging.h"

#include <limits>

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
    if (outcome.hasSubscription) {
        push::append_queuez_notification(scratch,
                                         before,
                                         outcome.subscription,
                                         key,
                                         nonce,
                                         response,
                                         written,
                                         after,
                                         armsRepush);
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
    } else if (outcome.hasEquipmentSwap) {
        // Body processing already staged this exact after-image so the correlated opcode-403
        // response could promise its version. Reuse it here; staging a second revision would make
        // the response and pushed Family-4 ladder disagree.
        const EquipmentSwap& swap = outcome.equipmentSwapUpdate;
        if (!valid(swap.after) || swap.characterSoid != outcome.equipmentSwap.characterSoid
            || swap.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || swap.after.family4Version != before.family4Version + 1
            || !push::append_equipment_swap_notification(
                scratch, swap, outcome.equipmentSwap, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=equip result=fail");
            // The State transaction must not commit when the Client cannot receive its after-image.
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = swap.after;
        // Family four drives inventory placement, while Family zero owns the rendered appearance
        // consumed by the open cosmetic panels and world player. Its resident character record is
        // updated in place: releasing and re-adding the same key tears down the ship/banner
        // binding.
        if (after.family0Active) {
            CharacterAppearanceRefresh refresh{};
            if (!stage_character_appearance_refresh(
                    after, outcome.equipmentSwap.characterSoid, refresh)
                || !push::append_equipment_appearance_refresh_notification(
                    scratch, refresh, outcome.equipmentSwap, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=equip_appearance result=fail");
                // The State transaction and both peer ladders remain unpublished when either
                // member of the paired Family-4/Family-0 delivery cannot fit.
                return false;
            }
            after = refresh.after;
        }
        // Family three owns the orbit roster and a separate copy of the same appearance record.
        // Equipment movement changes both bodies, so publish character first and roster second at
        // one exact +1 Family-3 revision.  Failure keeps all three peer ladders and State private.
        if (after.family3Active) {
            RosterAppearanceRefresh refresh{};
            if (!stage_roster_appearance_refresh(
                    after, outcome.equipmentSwap.characterSoid, true, refresh)
                || !push::append_equipment_roster_refresh_notification(
                    scratch, refresh, outcome.equipmentSwap, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=equip_roster result=fail");
                return false;
            }
            after = refresh.after;
        }
    } else if (outcome.hasItemState) {
        // Item-state bits live in the selected-character inventory row. Publish only that
        // resident character body; item-instance, appearance, roster and manifest are unchanged.
        const EquipmentSwap& update = outcome.itemStateUpdate;
        bool preservedManifest = update.after.family4ResidentCount == before.family4ResidentCount;
        for (std::size_t index = 0; preservedManifest && index < before.family4ResidentCount;
             ++index) {
            preservedManifest = update.after.family4Residents[index].objectSoid
                                    == before.family4Residents[index].objectSoid
                                && update.after.family4Residents[index].definitionId
                                       == before.family4Residents[index].definitionId;
        }
        if (!valid(update.after) || !preservedManifest
            || update.characterSoid != outcome.itemState.characterSoid
            || update.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || update.after.family4Version != before.family4Version + 1
            || !push::append_item_state_notification(
                scratch, update, outcome.itemState, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=item_state result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = update.after;
    } else if (outcome.hasSocketPlug) {
        // Body processing staged this exact +1 revision before encoding opcode 903's status pair.
        // A socket selection changes only one already-resident item-instance body.
        const SocketPlug& socketPlug = outcome.socketPlugUpdate;
        bool preservedManifest =
            socketPlug.after.family4ResidentCount == before.family4ResidentCount;
        std::size_t accountMatches = 0;
        std::size_t targetMatches = 0;
        for (std::size_t index = 0; preservedManifest && index < before.family4ResidentCount;
             ++index) {
            const ResidentObject& resident = before.family4Residents[index];
            const ResidentObject& staged = socketPlug.after.family4Residents[index];
            preservedManifest = staged.objectSoid == resident.objectSoid
                                && staged.definitionId == resident.definitionId;
            targetMatches += static_cast<std::size_t>(
                resident.objectSoid == socketPlug.targetInstanceSoid
                && resident.definitionId == socketPlug.itemInstanceDefinitionId);
            accountMatches += static_cast<std::size_t>(resident.objectSoid == socketPlug.accountSoid
                                                       && resident.definitionId
                                                              == socketPlug.accountDefinitionId);
        }
        if (!valid(socketPlug.after) || !preservedManifest || accountMatches != 1
            || targetMatches != 1 || socketPlug.accountSoid != outcome.socketPlug.accountSoid
            || socketPlug.characterSoid != outcome.socketPlug.characterSoid
            || socketPlug.targetInstanceSoid != outcome.socketPlug.targetInstanceSoid
            || socketPlug.updatesAccount != outcome.socketPlug.profileChanged
            || socketPlug.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || socketPlug.after.family4Version != before.family4Version + 1
            || !push::append_socket_plug_notification(
                scratch, socketPlug, outcome.socketPlug, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=socket_plug result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = socketPlug.after;
        // Equipped plugs feed Family zero's material, overflow-hash and sandbox-perk banks. An
        // inventory-only socket change has no rendered character record to refresh.
        if (outcome.socketPlug.targetEquipped && after.family0Active) {
            CharacterAppearanceRefresh refresh{};
            if (!stage_character_appearance_refresh(
                    after, outcome.socketPlug.characterSoid, refresh)
                || !push::append_socket_appearance_refresh_notification(
                    scratch, refresh, outcome.socketPlug, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=socket_appearance result=fail");
                return false;
            }
            after = refresh.after;
        }
        // A socket change can alter the rendered shader/perk banks in Family three, but it does not
        // change the roster's base-definition references.  Only the character record is owed.
        if (outcome.socketPlug.targetEquipped && after.family3Active) {
            RosterAppearanceRefresh refresh{};
            if (!stage_roster_appearance_refresh(
                    after, outcome.socketPlug.characterSoid, false, refresh)
                || !push::append_socket_roster_refresh_notification(
                    scratch, refresh, outcome.socketPlug, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=socket_roster result=fail");
                return false;
            }
            after = refresh.after;
        }
    } else if (outcome.hasItemAcquisition) {
        // Body processing staged this exact manifest append before encoding the response version.
        // The character and new item objects must both fit or the State insertion is not committed.
        const ItemAcquisition& acquisition = outcome.itemAcquisitionUpdate;
        const std::size_t appendedIndex = before.family4ResidentCount;
        bool preservedManifest = acquisition.after.family4ResidentCount == appendedIndex + 1U;
        for (std::size_t index = 0; preservedManifest && index < appendedIndex; ++index) {
            preservedManifest = acquisition.after.family4Residents[index].objectSoid
                                    == before.family4Residents[index].objectSoid
                                && acquisition.after.family4Residents[index].definitionId
                                       == before.family4Residents[index].definitionId;
        }
        if (!valid(acquisition.after) || !preservedManifest
            || acquisition.accountSoid != outcome.itemAcquisition.accountSoid
            || acquisition.characterSoid != outcome.itemAcquisition.characterSoid
            || acquisition.acquiredInstanceSoid != outcome.itemAcquisition.acquiredInstanceSoid
            || acquisition.updatesAccount != outcome.itemAcquisition.profileChanged
            || acquisition.accountSoid != before.family4RootSoid
            || acquisition.after.family4RootSoid != before.family4RootSoid
            || before.family4ResidentCount >= before.family4Residents.size()
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || acquisition.after.family4Version != before.family4Version + 1
            || acquisition.after.family4Residents[appendedIndex].objectSoid
                   != acquisition.acquiredInstanceSoid
            || acquisition.after.family4Residents[appendedIndex].definitionId
                   != acquisition.itemInstanceDefinitionId
            || !push::append_item_acquisition_notification(
                scratch, acquisition, outcome.itemAcquisition, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=acquire result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = acquisition.after;
    } else if (outcome.hasProfileItemAcquisition) {
        // A source-backed profile append creates one dependency before the account starts naming
        // it. Existing stacks and non-actionable currency rows preserve the complete manifest.
        const ProfileItemAcquisition& acquisition = outcome.profileItemAcquisitionUpdate;
        const std::size_t priorResidentCount = before.family4ResidentCount;
        const std::size_t expectedResidentCount =
            priorResidentCount + static_cast<std::size_t>(acquisition.appendedResident);
        bool validManifest = expectedResidentCount <= acquisition.after.family4Residents.size()
                             && acquisition.after.family4ResidentCount == expectedResidentCount;
        for (std::size_t index = 0; validManifest && index < priorResidentCount; ++index) {
            validManifest = acquisition.after.family4Residents[index].objectSoid
                                == before.family4Residents[index].objectSoid
                            && acquisition.after.family4Residents[index].definitionId
                                   == before.family4Residents[index].definitionId;
        }
        std::size_t priorProfileResidentMatches = 0;
        for (std::size_t index = 0; index < priorResidentCount; ++index) {
            const ResidentObject& resident = before.family4Residents[index];
            priorProfileResidentMatches += static_cast<std::size_t>(
                acquisition.acquiredInstanceSoid != 0
                && resident.objectSoid == acquisition.acquiredInstanceSoid
                && resident.definitionId == acquisition.itemInstanceDefinitionId);
        }
        const bool appendedResidentValid =
            !acquisition.appendedResident
            || (priorResidentCount < acquisition.after.family4Residents.size()
                && acquisition.after.family4Residents[priorResidentCount].objectSoid
                       == acquisition.acquiredInstanceSoid
                && acquisition.after.family4Residents[priorResidentCount].definitionId
                       == acquisition.itemInstanceDefinitionId
                && priorProfileResidentMatches == 0);
        const bool sourceIdentityValid =
            acquisition.actionSource == (acquisition.acquiredInstanceSoid != 0)
            && (acquisition.actionSource
                    ? acquisition.itemInstanceDefinitionId != 0 && appendedResidentValid
                          && (acquisition.appendedResident || priorProfileResidentMatches == 1)
                    : acquisition.itemInstanceDefinitionId == 0 && !acquisition.appendedResident
                          && priorProfileResidentMatches == 0);
        if (!valid(acquisition.after) || !validManifest || !sourceIdentityValid
            || acquisition.accountSoid != outcome.profileItemAcquisition.accountSoid
            || acquisition.acquiredInstanceSoid
                   != outcome.profileItemAcquisition.acquiredInstanceSoid
            || acquisition.actionSource != outcome.profileItemAcquisition.actionSource
            || acquisition.appendedResident
                   != (outcome.profileItemAcquisition.appended
                       && outcome.profileItemAcquisition.actionSource)
            || acquisition.accountSoid != before.family4RootSoid || before.family4ResidentCount == 0
            || acquisition.accountDefinitionId != before.family4Residents.front().definitionId
            || acquisition.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || acquisition.after.family4Version != before.family4Version + 1
            || !push::append_profile_item_acquisition_notification(scratch,
                                                                   acquisition,
                                                                   outcome.profileItemAcquisition,
                                                                   key,
                                                                   nonce,
                                                                   response,
                                                                   written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=profile_acquire result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = acquisition.after;
    } else if (outcome.hasItemDismantle) {
        // A dismantle removes exactly one resident while preserving the relative order of every
        // survivor. The character after-image and empty release descriptor must fit together or
        // the State removal is not committed.
        const ItemDismantle& dismantle = outcome.itemDismantleUpdate;
        bool compactedManifest =
            before.family4ResidentCount != 0
            && dismantle.after.family4ResidentCount + 1U == before.family4ResidentCount;
        std::size_t afterIndex = 0;
        std::size_t removedCount = 0;
        for (std::size_t beforeIndex = 0;
             compactedManifest && beforeIndex < before.family4ResidentCount;
             ++beforeIndex) {
            const ResidentObject& resident = before.family4Residents[beforeIndex];
            if (resident.objectSoid == dismantle.dismantledInstanceSoid) {
                compactedManifest = resident.definitionId == dismantle.itemInstanceDefinitionId;
                ++removedCount;
                continue;
            }
            if (afterIndex >= dismantle.after.family4ResidentCount) {
                compactedManifest = false;
                break;
            }
            const ResidentObject& survivor = dismantle.after.family4Residents[afterIndex++];
            compactedManifest = survivor.objectSoid == resident.objectSoid
                                && survivor.definitionId == resident.definitionId;
        }
        compactedManifest = compactedManifest && removedCount == 1U
                            && afterIndex == dismantle.after.family4ResidentCount;

        if (!valid(dismantle.after) || !compactedManifest
            || dismantle.accountSoid != outcome.itemDismantle.accountSoid
            || dismantle.characterSoid != outcome.itemDismantle.characterSoid
            || dismantle.dismantledInstanceSoid != outcome.itemDismantle.dismantledInstanceSoid
            || dismantle.updatesAccount != outcome.itemDismantle.profileChanged
            || dismantle.accountSoid != before.family4RootSoid
            || before.family4ResidentCount == 0
            || dismantle.accountDefinitionId != before.family4Residents.front().definitionId
            || dismantle.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || dismantle.after.family4Version != before.family4Version + 1
            || !push::append_item_dismantle_notification(
                scratch, dismantle, outcome.itemDismantle, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=dismantle result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = dismantle.after;
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
    return true;
}

} // namespace sunrise::server::bap::encrypted::queuez
