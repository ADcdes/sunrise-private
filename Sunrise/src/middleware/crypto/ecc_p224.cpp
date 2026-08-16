#include "ecc_p224.h"

#include <Windows.h>

#include <algorithm>
#include <bcrypt.h>
#include <vector>

namespace sunrise::middleware::crypto::ecc {

namespace {

/** DER tag for a SEQUENCE. */
constexpr std::byte kTagSequence{0x30};
/** DER tag for an INTEGER. */
constexpr std::byte kTagInteger{0x02};
/** DER tag for a BIT STRING. */
constexpr std::byte kTagBitString{0x03};
/** The key encoding opens with a one-bit flag that is set only on a private key. */
constexpr std::array<std::byte, 4> kPublicFlagBits{
    kTagBitString, std::byte{0x02}, std::byte{0x07}, std::byte{0x00}};
/** A DER length below this fits in the single byte that follows the tag. */
constexpr std::size_t kShortFormLimit = 0x80;
/** Bits in one byte. */
constexpr unsigned kByteBits = 8;
/** Public key blobs name the curve rather than carry its parameters. */
constexpr ULONG kGenericPublicMagic = 0x504B4345;

/** @return True for a BCrypt status that reports success. */
[[nodiscard]] bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

/**
 * Appends one positive integer in the minimal form DER requires.
 * A leading zero goes in when the top bit is set, so the value never reads as negative.
 * @param output Growing encoding.
 * @param value Field element, high byte first.
 */
void append_integer(std::vector<std::byte>& output, std::span<const std::byte> value) noexcept {
    std::size_t first = 0;
    while (first + 1 < value.size() && value[first] == std::byte{0}) {
        ++first;
    }
    const std::span<const std::byte> trimmed = value.subspan(first);
    const bool pad = (std::to_integer<unsigned>(trimmed[0]) & 0x80U) != 0;
    output.push_back(kTagInteger);
    output.push_back(static_cast<std::byte>(trimmed.size() + (pad ? 1U : 0U)));
    if (pad) {
        output.push_back(std::byte{0});
    }
    output.insert(output.end(), trimmed.begin(), trimmed.end());
}

/**
 * Encodes one public key the way the peer's importer reads it.
 * @param x Affine x, high byte first.
 * @param y Affine y, high byte first.
 * @param output Receives the encoding, zero padded to the fixed field.
 * @return True when the encoding fits the field.
 */
[[nodiscard]] bool encode_public_key(std::span<const std::byte> x,
                                     std::span<const std::byte> y,
                                     std::array<std::byte, kExportedKeySize>& output) noexcept {
    std::vector<std::byte> body;
    body.insert(body.end(), kPublicFlagBits.begin(), kPublicFlagBits.end());
    // The curve is identified by its field size, encoded as a plain integer.
    body.push_back(kTagInteger);
    body.push_back(std::byte{1});
    body.push_back(static_cast<std::byte>(kFieldSize));
    append_integer(body, x);
    append_integer(body, y);
    if (body.size() >= kShortFormLimit || body.size() + 2 > kExportedKeySize) {
        return false;
    }
    output = {};
    output[0] = kTagSequence;
    output[1] = static_cast<std::byte>(body.size());
    std::copy(body.begin(), body.end(), output.begin() + 2);
    return true;
}

/**
 * Reads one DER element header.
 * @param input Encoding.
 * @param cursor Position of the tag; advanced past the header on success.
 * @param tag Tag the element must carry.
 * @param length Receives the content length.
 * @return True when the element is present and its content fits the input.
 */
[[nodiscard]] bool read_header(std::span<const std::byte> input,
                               std::size_t& cursor,
                               std::byte tag,
                               std::size_t& length) noexcept {
    if (cursor + 2 > input.size() || input[cursor] != tag) {
        return false;
    }
    const auto declared = std::to_integer<std::size_t>(input[cursor + 1]);
    // Only the short form is accepted. Every element here is far below the long-form threshold.
    if (declared >= kShortFormLimit || cursor + 2 + declared > input.size()) {
        return false;
    }
    cursor += 2;
    length = declared;
    return true;
}

/**
 * Reads one integer into a fixed field, right aligned.
 * @param input Encoding.
 * @param cursor Position of the tag; advanced past the element on success.
 * @param output Receives the value, high byte first.
 * @return True when the element is an integer no wider than the field.
 */
[[nodiscard]] bool read_field_integer(std::span<const std::byte> input,
                                      std::size_t& cursor,
                                      std::array<std::byte, kFieldSize>& output) noexcept {
    std::size_t length = 0;
    if (!read_header(input, cursor, kTagInteger, length) || length == 0) {
        return false;
    }
    std::span<const std::byte> value = input.subspan(cursor, length);
    if (!value.empty() && value[0] == std::byte{0}) {
        value = value.subspan(1);
    }
    if (value.size() > kFieldSize) {
        return false;
    }
    output = {};
    std::copy(value.begin(), value.end(), output.end() - static_cast<std::ptrdiff_t>(value.size()));
    cursor += length;
    return true;
}

/**
 * Reads the peer's exported public key.
 * @param input Peer key, padding included.
 * @param x Receives affine x.
 * @param y Receives affine y.
 * @return True when the encoding is a public key on this curve.
 */
[[nodiscard]] bool decode_public_key(std::span<const std::byte> input,
                                     std::array<std::byte, kFieldSize>& x,
                                     std::array<std::byte, kFieldSize>& y) noexcept {
    std::size_t cursor = 0;
    std::size_t length = 0;
    if (!read_header(input, cursor, kTagSequence, length)) {
        return false;
    }
    if (!read_header(input, cursor, kTagBitString, length)) {
        return false;
    }
    cursor += length;
    std::size_t sizeLength = 0;
    if (!read_header(input, cursor, kTagInteger, sizeLength) || sizeLength != 1
        || input[cursor] != static_cast<std::byte>(kFieldSize)) {
        return false;
    }
    cursor += sizeLength;
    return read_field_integer(input, cursor, x) && read_field_integer(input, cursor, y);
}

/**
 * Opens the agreement algorithm bound to this curve.
 * @param output Receives the provider handle only on success.
 * @return True when Windows offers the curve.
 */
[[nodiscard]] bool open_provider(BCRYPT_ALG_HANDLE& output) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!succeeded(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDH_ALGORITHM, nullptr, 0))) {
        return false;
    }
    const auto* curve = reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_ECC_CURVE_SECP224R1));
    if (!succeeded(BCryptSetProperty(algorithm,
                                     BCRYPT_ECC_CURVE_NAME,
                                     const_cast<PUCHAR>(curve),
                                     sizeof(BCRYPT_ECC_CURVE_SECP224R1),
                                     0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    output = algorithm;
    return true;
}

/**
 * Imports the peer's point as a public key.
 * @param algorithm Provider bound to the curve.
 * @param x Affine x.
 * @param y Affine y.
 * @param output Receives the key handle only on success.
 * @return True when Windows accepted the point.
 */
[[nodiscard]] bool import_peer(BCRYPT_ALG_HANDLE algorithm,
                               const std::array<std::byte, kFieldSize>& x,
                               const std::array<std::byte, kFieldSize>& y,
                               BCRYPT_KEY_HANDLE& output) noexcept {
    std::array<std::byte, sizeof(BCRYPT_ECCKEY_BLOB) + (2 * kFieldSize)> blob{};
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = kGenericPublicMagic;
    header->cbKey = static_cast<ULONG>(kFieldSize);
    std::copy(x.begin(), x.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    std::copy(y.begin(), y.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB) + kFieldSize);
    return succeeded(BCryptImportKeyPair(algorithm,
                                         nullptr,
                                         BCRYPT_ECCPUBLIC_BLOB,
                                         &output,
                                         reinterpret_cast<PUCHAR>(blob.data()),
                                         static_cast<ULONG>(blob.size()),
                                         0));
}

/**
 * Runs the agreement and takes the raw secret.
 * @param ours Our private key.
 * @param theirs Peer's public key.
 * @param output Receives the x coordinate, high byte first.
 * @return True when Windows produced a full-width secret.
 */
[[nodiscard]] bool raw_secret(BCRYPT_KEY_HANDLE ours,
                              BCRYPT_KEY_HANDLE theirs,
                              std::array<std::byte, kFieldSize>& output) noexcept {
    BCRYPT_SECRET_HANDLE secret = nullptr;
    if (!succeeded(BCryptSecretAgreement(ours, theirs, &secret, 0))) {
        return false;
    }
    ULONG produced = 0;
    const bool derived = succeeded(BCryptDeriveKey(secret,
                                                   BCRYPT_KDF_RAW_SECRET,
                                                   nullptr,
                                                   reinterpret_cast<PUCHAR>(output.data()),
                                                   static_cast<ULONG>(output.size()),
                                                   &produced,
                                                   0));
    BCryptDestroySecret(secret);
    if (!derived || produced != output.size()) {
        // A short derive can still have written part of the secret.
        SecureZeroMemory(output.data(), output.size());
        return false;
    }
    // Windows hands the raw secret back low byte first; the peer reads it high byte first.
    std::reverse(output.begin(), output.end());
    return true;
}

} // namespace

/** Generates one key pair and agrees a secret with the peer's exported public key. */
bool agree(std::span<const std::byte> peerPublicKey, Agreement& output) noexcept {
    std::array<std::byte, kFieldSize> peerX{};
    std::array<std::byte, kFieldSize> peerY{};
    if (!decode_public_key(peerPublicKey, peerX, peerY)) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!open_provider(algorithm)) {
        return false;
    }

    BCRYPT_KEY_HANDLE ours = nullptr;
    BCRYPT_KEY_HANDLE theirs = nullptr;
    bool complete = false;
    if (succeeded(BCryptGenerateKeyPair(algorithm, &ours, kFieldSize * kByteBits, 0))
        && succeeded(BCryptFinalizeKeyPair(ours, 0))) {
        std::array<std::byte, sizeof(BCRYPT_ECCKEY_BLOB) + (2 * kFieldSize)> blob{};
        ULONG produced = 0;
        if (succeeded(BCryptExportKey(ours,
                                      nullptr,
                                      BCRYPT_ECCPUBLIC_BLOB,
                                      reinterpret_cast<PUCHAR>(blob.data()),
                                      static_cast<ULONG>(blob.size()),
                                      &produced,
                                      0))
            && produced == blob.size()) {
            const std::span<const std::byte> point{blob.data() + sizeof(BCRYPT_ECCKEY_BLOB),
                                                   2 * kFieldSize};
            complete =
                encode_public_key(point.first(kFieldSize), point.last(kFieldSize), output.publicKey)
                && import_peer(algorithm, peerX, peerY, theirs)
                && raw_secret(ours, theirs, output.sharedSecret);
        }
    }

    if (theirs != nullptr) {
        BCryptDestroyKey(theirs);
    }
    if (ours != nullptr) {
        BCryptDestroyKey(ours);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!complete) {
        SecureZeroMemory(output.sharedSecret.data(), output.sharedSecret.size());
        output = {};
    }
    return complete;
}

} // namespace sunrise::middleware::crypto::ecc
