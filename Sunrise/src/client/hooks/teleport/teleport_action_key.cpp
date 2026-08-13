/**
 * Turns an authored action binding into the Windows key the game will be asked about.
 *
 * A binding holds no virtual key. It holds an index into the game's own 105-entry key table, which
 * the per-frame scan reads through two image tables: a virtual key per index, and a scan code used
 * instead when that byte is the absent marker. Both are read here so an injected press matches
 * whatever the player bound, on any keyboard layout.
 */

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "internal.h"
#include "runtime.h"

namespace sunrise::client::hooks::teleport {
namespace {

/** The polled keyboard scan. Anchored on its prologue and the input-disable byte it loads first. */
constexpr std::string_view kScanSignatureText =
    "48 89 5C 24 18 48 89 6C 24 20 48 89 4C 24 08 56 57 41 55 41 56 41 57 48 83 EC 20 "
    "44 0F B6 3D ? ? ? ?";
/** Compiled pattern bytes of the scan signature. */
constexpr auto kScanSignature = signature<signature_length(kScanSignatureText)>(kScanSignatureText);

/** `movzx edi, byte ptr [r14+rax+disp32]`, the load of the virtual-key table. */
constexpr std::array<std::byte, 5> kVirtualKeyLoad{
    std::byte{0x41}, std::byte{0x0F}, std::byte{0xB6}, std::byte{0xBC}, std::byte{0x06}};
/** `movzx ecx, byte ptr [r14+rax+disp32]`, the load of the scan-code table. */
constexpr std::array<std::byte, 5> kScanCodeLoad{
    std::byte{0x41}, std::byte{0x0F}, std::byte{0xB6}, std::byte{0x8C}, std::byte{0x06}};

/** Bytes of the scan searched for both loads. Its loop body sits well inside this. */
constexpr std::size_t kSearchBytes = 0x140;
/** Both tables carry one byte per supported key index. */
constexpr std::size_t kKeyTableCount = 105;
/** The table byte meaning the index resolves through its scan code instead. */
constexpr std::uint8_t kAbsentVirtualKey = 0xFF;

const std::uint8_t* g_virtualKeys{};
const std::uint8_t* g_scanCodes{};

/**
 * Finds one image-base-relative table addressed by a load inside the scan.
 * @param scan Base of the scan function.
 * @param opcode The 5 opcode bytes before the displacement.
 * @return The table, or null when the load is not present.
 */
[[nodiscard]] const std::uint8_t* table_from(std::byte* scan,
                                             const std::array<std::byte, 5>& opcode) noexcept {
    auto* const base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return nullptr;
    }
    for (std::size_t offset = 0; offset + opcode.size() + sizeof(std::uint32_t) < kSearchBytes;
         ++offset) {
        std::byte* const site = scan + offset;
        if (std::memcmp(site, opcode.data(), opcode.size()) != 0) {
            continue;
        }
        std::uint32_t displacement = 0;
        std::memcpy(&displacement, site + opcode.size(), sizeof displacement);
        return reinterpret_cast<const std::uint8_t*>(base + displacement);
    }
    return nullptr;
}

} // namespace

/** Finds both key tables from the polled keyboard scan. */
bool resolve_action_keys() noexcept {
    std::byte* const scan = scan_main_image_unique(kScanSignature, "teleport_key_scan");
    if (scan == nullptr) {
        return false;
    }
    g_virtualKeys = table_from(scan, kVirtualKeyLoad);
    g_scanCodes = table_from(scan, kScanCodeLoad);
    return g_virtualKeys != nullptr && g_scanCodes != nullptr;
}

/** Drops both key tables. */
void clear_action_keys() noexcept {
    g_virtualKeys = nullptr;
    g_scanCodes = nullptr;
}

/** Turns one authored binding index into the virtual key the scan will read. */
std::uint32_t action_key(std::uint16_t index) noexcept {
    if (g_virtualKeys == nullptr || g_scanCodes == nullptr || index >= kKeyTableCount) {
        return 0;
    }
    const std::uint8_t direct = g_virtualKeys[index];
    if (direct != kAbsentVirtualKey) {
        return direct;
    }
    // The marker means the key moves with the layout, so we map it the way the scan does.
    const UINT mapped = MapVirtualKeyExA(g_scanCodes[index], MAPVK_VSC_TO_VK, GetKeyboardLayout(0));
    return mapped;
}

} // namespace sunrise::client::hooks::teleport
