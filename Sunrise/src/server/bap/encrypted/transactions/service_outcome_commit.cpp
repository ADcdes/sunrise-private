#include "service_outcome_commit.h"

#include "../../../../core/logging/log.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/matchmaking/matchmaking_state.h"
#include "../../../../state/runtime/runtime.h"
#include "../internal.h"

namespace sunrise::server::bap::encrypted::transactions {

/**
 * Commits at most one delayed State transaction.
 * @param outcome Checked service result whose pending transaction is used up.
 * @param publication Gets connection fields to publish after the output copy.
 * @return True when there is no transaction, or the one transaction commits.
 */
bool commit(ServiceOutcome& outcome, Publication& publication) noexcept {
    publication = {};
    const unsigned mutationCount = static_cast<unsigned>(outcome.hasActivitySessionAllocation)
                                   + static_cast<unsigned>(outcome.hasActivityTransaction)
                                   + static_cast<unsigned>(outcome.hasMatchmakingMutation)
                                   + static_cast<unsigned>(outcome.hasEquipmentSwap)
                                   + static_cast<unsigned>(outcome.hasSocketPlug)
                                   + static_cast<unsigned>(outcome.hasItemState)
                                   + static_cast<unsigned>(outcome.hasItemAcquisition)
                                   + static_cast<unsigned>(outcome.hasProfileItemAcquisition)
                                   + static_cast<unsigned>(outcome.hasItemDismantle);
    // A service route may never combine independently versioned State transactions.
    if (mutationCount > 1U) {
        return false;
    }
    if (outcome.hasActivitySessionAllocation) {
        const std::uint64_t sessionId = outcome.activitySessionAllocation.sessionId;
        if (sessionId == state::activity::kAbsentSessionId
            || !state::activity::commit(outcome.activitySessionAllocation)) {
            return false;
        }
        publication.activitySessionId = sessionId;
        publication.hasActivitySessionBinding = true;
        return true;
    }
    if (outcome.hasActivityTransaction) {
        if (outcome.activityPlan.mutationDomain == activity_message::MutationDomain::entitySlots) {
            return state::activity::entity_slots::commit(outcome.activityPlan.entitySlotMutation);
        }
        if (outcome.activityPlan.mutationDomain == activity_message::MutationDomain::membership) {
            return state::activity::membership::commit(outcome.activityPlan.membershipMutation);
        }
        // The retained patch epoch is connection state, so it commits nothing here.
        return outcome.activityPlan.mutationDomain == activity_message::MutationDomain::patchEpoch;
    }
    if (outcome.hasMatchmakingMutation) {
        return state::matchmaking::commit(outcome.matchmakingMutation);
    }
    if (outcome.hasEquipmentSwap) {
        const bool committed = state::commit_equipment_swap(outcome.equipmentSwap);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=equip stage=transaction_commit result=ok"
                                   : "ev=equip stage=transaction_commit result=fail");
        return committed;
    }
    if (outcome.hasItemAcquisition) {
        const bool committed = state::commit_item_acquisition(outcome.itemAcquisition);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=acquire stage=transaction_commit result=ok"
                                   : "ev=acquire stage=transaction_commit result=fail");
        return committed;
    }
    if (outcome.hasSocketPlug) {
        const bool committed = state::commit_socket_plug(outcome.socketPlug);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=socket_plug stage=transaction_commit result=ok"
                                   : "ev=socket_plug stage=transaction_commit result=fail");
        return committed;
    }
    if (outcome.hasItemState) {
        const bool committed = state::commit_item_state(outcome.itemState);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=item_state stage=transaction_commit result=ok"
                                   : "ev=item_state stage=transaction_commit result=fail");
        return committed;
    }
    if (outcome.hasProfileItemAcquisition) {
        const bool committed =
            state::commit_profile_item_acquisition(outcome.profileItemAcquisition);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=profile_acquire stage=transaction_commit result=ok"
                                   : "ev=profile_acquire stage=transaction_commit result=fail");
        return committed;
    }
    if (outcome.hasItemDismantle) {
        const bool committed = state::commit_item_dismantle(outcome.itemDismantle);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=dismantle stage=transaction_commit result=ok"
                                   : "ev=dismantle stage=transaction_commit result=fail");
        return committed;
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::transactions
