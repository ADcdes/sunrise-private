#include "service_outcome_commit.h"

#include "../../../../state/activity/bubble_authority/runtime.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/matchmaking/matchmaking_state.h"
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
                                   + static_cast<unsigned>(outcome.hasMatchmakingMutation);
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
            if (!state::activity::entity_slots::commit(outcome.activityPlan.entitySlotMutation)) {
                return false;
            }
            // The keepalive only finds a link that is bound to a session. A link that allocated
            // its own session carries the same id, so this rebinds it to itself.
            if (outcome.activityPlan.delivery == activity_message::Delivery::joinNotifications
                && outcome.activityPlan.sessionId != state::activity::kAbsentSessionId) {
                publication.activitySessionId = outcome.activityPlan.sessionId;
                publication.hasActivitySessionBinding = true;
                publication.activitySessionFromJoin = true;
                // The join resets the client's roster container and the grant mirror with it.
                // A kept grant leaves that container ungranted, which refuses its placed objects.
                state::activity::bubble_authority::clear_grants(outcome.activityPlan.sessionId);
            }
            return true;
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
    return true;
}

} // namespace sunrise::server::bap::encrypted::transactions
