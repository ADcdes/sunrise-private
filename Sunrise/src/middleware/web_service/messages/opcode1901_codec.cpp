#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode1901.h"

namespace sunrise::middleware::web_service::messages::opcode1901 {
namespace {

/** The reflected opcode-1901 request occupies exactly 192 bits. */
constexpr std::size_t kPayloadSize = 24;
/** The native fixed replacement array reserves twelve entries, so its count uses four bits. */
constexpr std::uint8_t kReplacementCountWidth = 4;
/** This codec supports the single replacement emitted by one shader Apply action. */
constexpr std::uint64_t kCanonicalReplacementCount = 1;
/** Signed native definition indices use one presence bit followed by fifteen value bits. */
constexpr std::uint8_t kDefinitionIndexWidth = 15;
/** The canonical socket-kind byte is signed and biased from INT8_MIN. */
constexpr std::uint8_t kCanonicalSocketKindWidth = 8;
/** The compact model socket-kind discriminator occupies two bits. */
constexpr std::uint8_t kModelSocketKindWidth = 2;
/** The signed socket index is biased from INT32_MIN. */
constexpr std::uint8_t kSocketIndexWidth = 32;
/** The replacement auxiliary identity and semantic equipment selector are optional 64-bit fields.
 */
constexpr std::uint8_t kOptionalIdentityWidth = 64;
/** Nonnegative signed 8-bit values have this bit set after native descriptor biasing. */
constexpr std::uint64_t kCanonicalSocketKindBias = 0x80ULL;
/** Nonnegative signed 32-bit values have this bit set after native descriptor biasing. */
constexpr std::uint64_t kSocketIndexBias = 0x80000000ULL;
/** Model socket-kind zero is encoded as one so wire zero remains the absent sentinel. */
constexpr std::uint64_t kModelSocketKindBias = 1;
/** Shader Apply targets an ordinary item socket model. */
constexpr std::uint64_t kShaderModelSocketKind = 0;
/** Collection-backed shader action sources carry no auxiliary instance identity. */
constexpr std::uint64_t kShaderAuxiliary = 0;
/** The character screen's selector is four times the instance identity it names. */
constexpr std::uint64_t kSelectorStride = 4;

} // namespace

/** Compares a decoded selector against the part of an instance identity the wire carries. */
bool identifies_instance(std::uint64_t instanceIdentityToken,
                         std::uint64_t instanceSoid) noexcept {
    return instanceIdentityToken != 0
           && (instanceSoid & kInstanceIdentityMask) == instanceIdentityToken;
}

/** Parses the complete native equipped shader socket-action descriptor. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }

    encoding::bits::Reader reader(message.payload);
    std::uint64_t replacementCount = 0;
    std::uint64_t plugDefinitionPresent = 0;
    std::uint64_t encodedPlugDefinition = 0;
    std::uint64_t encodedCanonicalSocketKind = 0;
    std::uint64_t modelSocketKind = 0;
    std::uint64_t encodedSocketIndex = 0;
    std::uint64_t auxiliaryPresent = 0;
    std::uint64_t equipmentSelectorPresent = 0;
    if (!reader.read(kReplacementCountWidth, replacementCount)
        || !reader.read(1, plugDefinitionPresent)
        || !reader.read(kDefinitionIndexWidth, encodedPlugDefinition)
        || !reader.read(kCanonicalSocketKindWidth, encodedCanonicalSocketKind)
        || !reader.read(kModelSocketKindWidth, modelSocketKind)
        || !reader.read(kSocketIndexWidth, encodedSocketIndex) || !reader.read(1, auxiliaryPresent)
        || !reader.read(kOptionalIdentityWidth, request.auxiliary)
        || !reader.read(1, equipmentSelectorPresent)
        || !reader.read(kOptionalIdentityWidth, request.equipmentSelector)
        || reader.remaining_bits() != 0 || replacementCount != kCanonicalReplacementCount
        || plugDefinitionPresent == 0 || auxiliaryPresent == 0 || equipmentSelectorPresent == 0
        || encodedCanonicalSocketKind < kCanonicalSocketKindBias
        || modelSocketKind < kModelSocketKindBias || encodedSocketIndex < kSocketIndexBias
        || modelSocketKind - kModelSocketKindBias != kShaderModelSocketKind
        || request.auxiliary != kShaderAuxiliary) {
        request = {};
        return false;
    }

    // The selector is a wire encoding of an instance identity, so it is decoded here rather than
    // handed on as a raw number for a later layer to divide.
    if (request.equipmentSelector == 0 || request.equipmentSelector % kSelectorStride != 0
        || request.equipmentSelector / kSelectorStride > kInstanceIdentityMask) {
        const std::uint64_t selector = request.equipmentSelector;
        request = {};
        request.equipmentSelector = selector;
        return false;
    }
    request.instanceIdentityToken = request.equipmentSelector / kSelectorStride;

    request.plugDefinitionIndex = static_cast<std::uint16_t>(encodedPlugDefinition);
    request.canonicalSocketKind =
        static_cast<std::uint8_t>(encodedCanonicalSocketKind - kCanonicalSocketKindBias);
    request.modelSocketKind = static_cast<std::uint8_t>(modelSocketKind - kModelSocketKindBias);
    request.socketIndex = static_cast<std::uint32_t>(encodedSocketIndex - kSocketIndexBias);
    return request.canonicalSocketKind == request.socketIndex;
}

} // namespace sunrise::middleware::web_service::messages::opcode1901
