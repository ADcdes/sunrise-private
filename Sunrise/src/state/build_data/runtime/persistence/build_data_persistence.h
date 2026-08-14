#pragma once

#include <Windows.h>

#include <array>
#include <memory>

#include "../../../../core/filesystem/path.h"
#include "../../../content/content_catalog.h"
#include "../../abilities/definition.h"
#include "../../cache/records/domains.h"
#include "../../collectibles/collectible_catalog.h"
#include "../../constants/definition.h"
#include "../../definition.h"
#include "../../hash_names/definition.h"
#include "../../inventory/buckets/definition.h"
#include "../../items/details/definition.h"
#include "../../items/item_catalog.h"
#include "../../items/socket_plugs/definition.h"
#include "../../material_requirements/material_requirement_catalog.h"
#include "../../progressions/definition.h"
#include "../../scenarios/definition.h"
#include "../../socket_entry_lists/definition.h"
#include "../../spawn_sets/definition.h"

namespace sunrise::state::build_data::runtime::persistence {

/** Fixed cache paths, identity, and canonical snapshot storage guarded by one State lock. */
struct Context {
    SRWLOCK lock{SRWLOCK_INIT};
    std::unique_ptr<content::Definition[]> namedScratch{};
    std::unique_ptr<items::Definition[]> itemScratch{};
    std::unique_ptr<collectibles::Definition[]> collectibleScratch{};
    std::unique_ptr<material_requirements::Definition[]> materialRequirementSetScratch{};
    std::unique_ptr<items::details::Definition[]> itemDetailScratch{};
    std::unique_ptr<items::socket_plugs::Rule[]> socketPlugRuleScratch{};
    std::unique_ptr<items::socket_plugs::Pool[]> socketPlugPoolScratch{};
    std::unique_ptr<items::socket_plugs::Member[]> socketPlugMemberScratch{};
    std::unique_ptr<inventory::buckets::Descriptor[]> inventoryBucketScratch{};
    std::unique_ptr<socket_entry_lists::Definition[]> socketEntryListScratch{};
    std::unique_ptr<socket_entry_lists::EntryTable[]> socketEntryTableScratch{};
    std::unique_ptr<abilities::Definition[]> abilityBucketScratch{};
    std::unique_ptr<progressions::Definition[]> progressionScratch{};
    std::unique_ptr<scenarios::Definition[]> scenarioScratch{};
    std::unique_ptr<scenarios::RosterGroup[]> rosterGroupScratch{};
    std::unique_ptr<spawn_sets::Stem[]> spawnStemScratch{};
    std::unique_ptr<spawn_sets::NameHash[]> spawnNameHashScratch{};
    std::unique_ptr<hash_names::Name[]> hashNameScratch{};
    cache::records::InvestmentConstants constantsScratch{};
    core::path::Buffer cacheDirectory;
    core::path::Buffer cachePath;
    BuildIdentity buildIdentity{};
    bool enabled{};
    bool persisted{};
    bool replaceStaleCache{};
};

/** @return The process-wide persistence context, shared by lifecycle and writer code. */
[[nodiscard]] Context& context() noexcept;

/** @return True when every generated domain is complete in State. */
[[nodiscard]] bool all_domains_ready() noexcept;

/**
 * Clears fixed cache paths, identity, flags, and snapshot storage.
 * @param state Its lock must already be held exclusively.
 */
void clear_locked(Context& state) noexcept;

/**
 * Gives mutable views over every generated domain.
 * @param state Its lock must already be held exclusively.
 * @return Mutable spans covering each full-size domain buffer.
 */
[[nodiscard]] cache::records::MutableDomains scratch_domains(Context& state) noexcept;

/**
 * Gives read-only views over the used rows of one canonical snapshot.
 * @param state Its lock must already be held exclusively.
 * @param counts Used row count for each fixed domain buffer.
 * @return Read-only spans limited to the used rows.
 */
[[nodiscard]] cache::records::Domains
occupied_domains(Context& state, const cache::records::DomainCounts& counts) noexcept;

/**
 * Saves one complete canonical snapshot when every domain is ready.
 * @param state Its lock must already be held exclusively.
 * @return True when no write is due, or the complete State is on disk.
 */
[[nodiscard]] bool persist_if_complete_locked(Context& state) noexcept;

} // namespace sunrise::state::build_data::runtime::persistence
