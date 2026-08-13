#include <array>

#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::abilities;

/** The authored equipment slot that holds the subclass. */
constexpr std::size_t kSubclassSlot =
    static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);

/**
 * Finds the socket entry list that carries one character's subclass abilities.
 * @param character Authored character.
 * @param socketEntryListIndex Receives the subclass's socket-entry-list index.
 * @return True when the character equips a subclass whose detail is published.
 */
[[nodiscard]] bool subclass_list(const state::CharacterState& character,
                                 std::uint16_t& socketEntryListIndex) noexcept {
    const auto& slot = character.equipment.slots[kSubclassSlot];
    state::build_data::items::Definition item{};
    state::build_data::items::details::Definition detail{};
    if (!slot.has_value()
        || !state::build_data::find_item_definition_hash(slot->definitionHash, item)
        || !state::build_data::find_configured_item_detail(item.definitionIndex, detail)) {
        return false;
    }
    socketEntryListIndex = detail.socketEntryListIndex;
    return true;
}

/** @param rows Rows built so far. @return True when the candidate's key is already held. */
[[nodiscard]] bool held(std::span<const domain::Definition> rows,
                        const domain::Definition& row) noexcept {
    for (const domain::Definition& existing : rows) {
        if (existing.socketEntryListIndex == row.socketEntryListIndex
            && existing.movementEntry == row.movementEntry) {
            return true;
        }
    }
    return false;
}

} // namespace

/** Builds one ability bucket row per distinct subclass and movement selection in use. */
bool build_character_abilities(const reader::Source& source,
                               reader::Scratch& scratch,
                               std::span<const std::byte> root,
                               std::vector<std::byte>& table,
                               std::vector<std::byte>& definition,
                               std::vector<std::byte>& blob,
                               std::span<state::build_data::abilities::Definition> output,
                               std::size_t& count) noexcept {
    count = 0;
    std::uint32_t tableTag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kSocketEntryListTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, scratch, tableTag, table)
        || !tables::find_array_at(
            std::span<const std::byte>{table}, tables::kTableArrayDescriptor, rows)) {
        return false;
    }
    const state::AccountState account = state::account_snapshot();
    for (std::size_t character = 0; character < account.characterCount && count < output.size();
         ++character) {
        domain::Definition row{};
        if (!subclass_list(account.characters[character], row.socketEntryListIndex)) {
            continue;
        }
        row.movementEntry = account.characters[character].movementAbilityEntry;
        if (held(output.first(count), row)) {
            continue;
        }
        tables::IndexRow indexRow{};
        if (!tables::index_row(
                std::span<const std::byte>{table}, rows, row.socketEntryListIndex, indexRow)
            || indexRow.targetTag == 0
            || !reader::read_tag(source, scratch, indexRow.targetTag, definition)
            || !build_ability_buckets(source,
                                      scratch,
                                      std::span<const std::byte>{definition},
                                      blob,
                                      row.movementEntry,
                                      row)) {
            continue;
        }
        output[count++] = row;
    }
    return true;
}

} // namespace sunrise::client::content::items::packages
