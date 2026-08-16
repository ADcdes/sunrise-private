#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode1901 {

/** Web Service opcode used by the equipped-item shader application action. */
inline constexpr std::uint16_t kOpcode = 1901;

/** The equipment selector carries only the low 62 bits of the item instance's identity. */
inline constexpr std::uint64_t kInstanceIdentityMask = 0x3FFFFFFFFFFFFFFFULL;

/** Exact logical fields carried by the native 192-bit equipped socket-action descriptor. */
struct Request {
    std::uint16_t plugDefinitionIndex{};
    std::uint8_t canonicalSocketKind{};
    std::uint8_t modelSocketKind{};
    std::uint32_t socketIndex{};
    std::uint64_t auxiliary{};
    /** Selector exactly as the descriptor carries it, kept for the request trace. */
    std::uint64_t equipmentSelector{};
    /** The item-instance identity that selector encodes, already decoded. */
    std::uint64_t instanceIdentityToken{};
};

/**
 * Answers whether one decoded selector names a given item instance.
 *
 * The selector carries only part of the instance identity, so naming an instance is a comparison
 * against that part rather than an equality. Callers hold whole identities and must not have to
 * know which part of one reaches the wire.
 *
 * @param instanceIdentityToken Decoded selector from a parsed request.
 * @param instanceSoid Whole item-instance identity held by State.
 * @return True when the selector names that instance.
 */
[[nodiscard]] bool identifies_instance(std::uint64_t instanceIdentityToken,
                                       std::uint64_t instanceSoid) noexcept;

/**
 * Parses the exact reflected opcode-1901 descriptor.
 *
 * The native request is a bounded replacement array followed by the target's semantic equipment
 * selector. The supported wire shape contains exactly one replacement and requires every optional
 * identity carried by that replacement and target.
 *
 * @param message Parsed Web Service envelope.
 * @param request Receives the plug, socket descriptor, auxiliary identity, and equipment slot.
 * @return True only for the complete canonical 24-byte one-replacement request.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode1901
