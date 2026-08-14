#include "../../runtime.h"
#include "../../runtime/persistence/publication_transaction.h"
#include "../details/item_detail_catalog.h"
#include "../item_catalog.h"
#include "socket_plug_catalog.h"

namespace sunrise::state::build_data {
namespace {

/** Checks every exact socket relation link against the already-published item details. */
[[nodiscard]] bool
valid_socket_plug_publication(std::span<const items::socket_plugs::Rule> rules,
                              std::span<const items::socket_plugs::Pool> pools,
                              std::span<const items::socket_plugs::Member> members) noexcept {
    if (!items::socket_plugs::valid(rules, pools, members) || items::count() == 0
        || !configured_item_details_ready()) {
        return false;
    }
    for (const items::socket_plugs::Rule& rule : rules) {
        items::details::Definition detail{};
        if (rule.itemDefinitionIndex >= items::count()
            || !items::details::find(rule.itemDefinitionIndex, detail)
            || rule.lane >= detail.ordinarySocketCount) {
            return false;
        }
    }
    for (const items::socket_plugs::Member member : members) {
        if (member >= items::count()) {
            return false;
        }
    }
    return true;
}

} // namespace

/** @return True when the installed exact ordinary-socket relation is in State. */
bool socket_plug_rules_ready() noexcept {
    return items::socket_plugs::rule_count() != 0;
}

/** Publishes the complete exact socket relation in one persistence transaction. */
bool publish_socket_plug_rules(std::span<const items::socket_plugs::Rule> rules,
                               std::span<const items::socket_plugs::Pool> pools,
                               std::span<const items::socket_plugs::Member> members) noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active() || !valid_socket_plug_publication(rules, pools, members)) {
        return false;
    }
    return transaction.finish(items::socket_plugs::replace(rules, pools, members),
                              items::socket_plugs::clear);
}

/** Answers one exact installed item/lane/plug compatibility query. */
bool is_socket_plug_allowed(std::uint16_t itemDefinitionIndex,
                            std::uint8_t lane,
                            std::uint16_t plugDefinitionIndex) noexcept {
    return socket_plug_rules_ready()
           && items::socket_plugs::allowed(itemDefinitionIndex, lane, plugDefinitionIndex);
}

/** Answers whether one installed profile row is a materializable socket action source. */
bool is_profile_action_source(std::uint16_t itemDefinitionIndex, std::uint8_t bucketId) noexcept {
    constexpr std::uint8_t kModBucketId = 13;
    constexpr std::uint8_t kShaderBucketId = 14;
    items::details::Definition detail{};
    inventory::buckets::Descriptor bucket{};
    if ((bucketId != kModBucketId && bucketId != kShaderBucketId) || !socket_plug_rules_ready()
        || !find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionIndex != itemDefinitionIndex || detail.bucketId != bucketId
        || !find_inventory_bucket_descriptor(bucketId, bucket)
        || bucket.arraySelector != inventory::buckets::ArraySelector::profile) {
        return false;
    }
    return detail.instancedDefinitionState == items::details::InstancedDefinitionState::stackable
           && items::socket_plugs::contains(itemDefinitionIndex);
}

} // namespace sunrise::state::build_data
