#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "middleware/content/packages/tables/definition_index_table.h"
#include "middleware/content/packages/tables/items.h"
#include "middleware/datagen/character_record/appearance/internal.h"
#include "middleware/queuez/queuez_update.h"
#include "middleware/web_service/messages/opcode1901.h"
#include "server/bap/encrypted/queuez/queuez_state_validation.h"
#include "server/web_service/web_service_runtime.h"
#include "state/build_data/inventory/buckets/inventory_bucket_catalog.h"
#include "state/build_data/table.h"

namespace {

int failures{};

void expect(bool value, const char* label) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

template <typename Value>
void write_at(std::span<std::byte> bytes, std::size_t offset, Value value) {
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

void test_opcode1901_fixture() {
    using namespace sunrise::middleware::web_service;
    using namespace sunrise::middleware::web_service::messages;
    constexpr std::array<std::uint8_t, 24> wire{
        0x19, 0xA8, 0x28, 0x56, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
    };
    Message message{
        opcode1901::kOpcode, 7, {reinterpret_cast<const std::byte*>(wire.data()), wire.size()}};
    opcode1901::Request decoded{};
    expect(opcode1901::parse_request(message, decoded), "opcode1901 captured fixture parses");
    expect(decoded.plugDefinitionIndex == 6786, "opcode1901 plug index");
    expect(decoded.canonicalSocketKind == 5 && decoded.modelSocketKind == 0,
           "opcode1901 socket kinds");
    expect(decoded.socketIndex == 5 && decoded.auxiliary == 0, "opcode1901 socket descriptor");
    expect(decoded.equipmentSelector == 8, "opcode1901 equipment selector");

    auto malformed = wire;
    malformed[0] = 0;
    message.payload = {reinterpret_cast<const std::byte*>(malformed.data()), malformed.size()};
    expect(!opcode1901::parse_request(message, decoded), "opcode1901 rejects wrong count");
    expect(decoded.plugDefinitionIndex == 0 && decoded.equipmentSelector == 0,
           "opcode1901 clears rejected output");
}

void test_class_art_selection() {
    namespace appearance = sunrise::middleware::datagen::character_record::appearance;
    namespace details = sunrise::state::build_data::items::details;
    details::Definition definition{};
    definition.artArrangementIndices = {100, 101, details::kUnavailableArtIndex, 103};
    expect(appearance::select_art_arrangement(definition, sunrise::state::CharacterClass::titan)
               == 101,
           "Titan art selects class row");
    expect(appearance::select_art_arrangement(definition, sunrise::state::CharacterClass::hunter)
               == 100,
           "missing Hunter art falls back to generic");
    expect(appearance::select_art_arrangement(definition, sunrise::state::CharacterClass::warlock)
               == 103,
           "Warlock art selects class row");
}

void test_class_art_extraction() {
    namespace tables = sunrise::middleware::content::packages::tables;
    namespace items = tables::items;
    std::array<std::byte, 384> blob{};
    constexpr std::size_t art = 192;
    constexpr std::size_t header = 304;
    constexpr std::size_t data = header + 16;
    write_at(
        blob, tables::kArtBlockOffset, static_cast<std::int64_t>(art - tables::kArtBlockOffset));
    write_at(blob, art, std::uint64_t{3});
    write_at(blob, art + 8, static_cast<std::int64_t>(header - (art + 8)));
    write_at(blob, header - 4, std::uint32_t{0x80800000});
    write_at(blob, header, std::uint64_t{3});
    write_at(blob, header + 8, tables::kArtRowClass);
    write_at(blob, art + 88, std::uint16_t{77});
    write_at(blob, data, std::int8_t{-1});
    write_at(blob, data + 2, std::uint16_t{100});
    write_at(blob, data + 4, std::int8_t{0});
    write_at(blob, data + 6, std::uint16_t{101});
    write_at(blob, data + 8, std::int8_t{2});
    write_at(blob, data + 10, std::uint16_t{103});

    items::Row row{};
    items::read_appearance(blob, row);
    expect(row.gearArtIndex == 77, "appearance extraction reads gear art");
    expect(row.artArrangementIndices[0] == 100 && row.artArrangementIndices[1] == 101
               && row.artArrangementIndices[2] == items::kUnavailableArtIndex
               && row.artArrangementIndices[3] == 103,
           "appearance extraction keeps generic and class rows");
}

void test_table_replacement() {
    struct Row {
        std::uint32_t value{};
    };
    sunrise::state::build_data::Table<Row, 8> table{};
    constexpr std::array<Row, 4> first{{{1}, {2}, {3}, {4}}};
    constexpr std::array<Row, 2> second{{{9}, {8}}};
    expect(table.replace(first), "table accepts first replacement");
    expect(table.replace(second), "table accepts shorter replacement");
    std::array<Row, 8> snapshot{};
    std::size_t count = 0;
    expect(table.snapshot(snapshot, count) && count == second.size(), "table publishes new count");
    expect(snapshot[0].value == second[0].value && snapshot[1].value == second[1].value,
           "table publishes new prefix");
    expect(table.reset(3).size() == 3 && table.count() == 3, "table reset resizes safely");
    table.clear();
    expect(table.count() == 0, "table clear publishes empty state");
}

void test_bucket_equipment_mapping_validation() {
    namespace buckets = sunrise::state::build_data::inventory::buckets;
    constexpr std::array<buckets::Descriptor, 2> mapped{{
        {0, buckets::ArraySelector::character, 0, 10, 7, 0},
        {13, buckets::ArraySelector::profile, 75, 50, buckets::kUnavailableEquipmentSlot, 0},
    }};
    expect(buckets::valid(mapped), "bucket table accepts a unique extracted equipment slot");

    auto unmapped = mapped;
    unmapped[0].equipmentSlot = buckets::kUnavailableEquipmentSlot;
    expect(!buckets::valid(unmapped), "bucket table rejects an all-unmapped cache domain");

    auto duplicate = mapped;
    duplicate[1].equipmentSlot = mapped[0].equipmentSlot;
    expect(!buckets::valid(duplicate), "bucket table rejects duplicate equipment slots");
}

void test_family4_resync() {
    namespace queuez = sunrise::server::bap::encrypted::queuez;
    namespace wire = sunrise::middleware::queuez;
    queuez::SessionState before{};
    before.family4RootSoid = 0x100;
    before.family4Version = 3;
    before.family4ResidentCount = 2;
    before.family4Active = true;
    before.family4Residents[0] = {0x100, 10};
    before.family4Residents[1] = {0x101, 11};
    expect(queuez::valid(before), "Family4 before state is valid");

    constexpr std::array<wire::Object, 3> objects{{
        {10, 0x100, wire::Encoding::oodle, {}},
        {11, 0x101, wire::Encoding::oodle, {}},
        {12, 0x202, wire::Encoding::oodle, {}},
    }};
    const wire::Family refresh{
        queuez::kAccountFamilyType, 0x100, 4, wire::kFullSnapshotFlag, objects};
    queuez::SessionState after{};
    expect(queuez::stage_family4_refresh(before, refresh, after),
           "Family4 next-version refresh stages");
    expect(after.family4Version == 4 && after.family4ResidentCount == 3,
           "Family4 refresh advances version and manifest");
    expect(after.family4Residents[2].objectSoid == 0x202, "Family4 refresh records added resident");

    wire::Family stale = refresh;
    stale.version = before.family4Version;
    queuez::SessionState rejected{};
    expect(!queuez::stage_family4_refresh(before, stale, rejected),
           "Family4 rejects stale refresh version");
    expect(rejected.family4Version == before.family4Version
               && rejected.family4ResidentCount == before.family4ResidentCount,
           "rejected refresh leaves exact before image");

    auto duplicateObjects = objects;
    duplicateObjects[2].version = duplicateObjects[1].version;
    wire::Family duplicate = refresh;
    duplicate.objects = duplicateObjects;
    expect(!queuez::stage_family4_refresh(before, duplicate, rejected),
           "Family4 rejects duplicate resident identity");
}

void test_outcome_exclusivity() {
    using namespace sunrise;
    server::web_service::Outcome outcome{};
    state::PendingItemState itemState{};
    itemState.prepared = true;
    outcome.mutation = itemState;
    expect(server::web_service::mutation_if<state::PendingItemState>(outcome) != nullptr,
           "Outcome stores item-state transaction");
    expect(server::web_service::mutation_if<state::PendingSocketPlug>(outcome) == nullptr,
           "Outcome exposes only one transaction alternative");
    state::PendingSocketPlug socket{};
    socket.prepared = true;
    outcome.mutation = socket;
    expect(server::web_service::mutation_if<state::PendingItemState>(outcome) == nullptr
               && server::web_service::mutation_if<state::PendingSocketPlug>(outcome) != nullptr,
           "Outcome replacement cannot retain a second mutation");
}

} // namespace

int main() {
    test_opcode1901_fixture();
    test_class_art_selection();
    test_class_art_extraction();
    test_table_replacement();
    test_bucket_equipment_mapping_validation();
    test_family4_resync();
    test_outcome_exclusivity();
    if (failures != 0) {
        std::fprintf(stderr, "%d regression test(s) failed\n", failures);
        return 1;
    }
    std::puts("All Sunrise regression tests passed");
    return 0;
}
