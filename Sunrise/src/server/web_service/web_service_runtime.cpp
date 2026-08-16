#include "web_service_runtime.h"

#include <array>
#include <chrono>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode205.h"
#include "../../middleware/web_service/messages/opcode206.h"
#include "../../middleware/web_service/messages/opcode501_codec.h"
#include "../../middleware/web_service/messages/opcode503.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode601/opcode601_codec.h"
#include "../../middleware/web_service/messages/opcode901/opcode901_codec.h"
#include "../../middleware/web_service/web_service_envelope.h"
#include "../../state/account/account_state.h"
#include "../../state/runtime/runtime.h"
#include "opcode_routes.h"

namespace sunrise::server::web_service {

/** One log line carries the opcode and its fixed prefix. */
constexpr std::size_t kOpcodeLineCapacity = 64;

/**
 * Logs which Web Service opcode arrived. One svc-10 frame looks like any other in the log, and
 * the opcodes the Client sends are what drive its queuez state machine.
 * @param opcode Parsed wire opcode.
 */
void report_opcode(std::uint32_t opcode) noexcept {
    std::array<char, kOpcodeLineCapacity> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=ws stage=request opcode=%u", opcode);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** One refusal line carries both request indices, the clock presence, and the clock verdict. */
constexpr std::size_t kPurchaseLineCapacity = 128;
/**
 * Status code answered to a purchase request.
 * Any non-zero value refuses. Zero is the success code, so it must not be used here.
 */
constexpr std::int32_t kPurchaseRefusedCode = 1;

/**
 * Reads the server's own clock for the purchase clock rule.
 * The system clock counts from the Unix epoch, which is the same base the request field uses.
 * @return Current time in Unix seconds.
 */
[[nodiscard]] std::int64_t server_clock_seconds() noexcept {
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch).count();
}

/**
 * Refuses one vendor purchase and answers it.
 * No award, cost or stock rule exists yet, so no purchase can succeed. The refusal must still be
 * answered, because no answer holds the head of the client's pending queue.
 * @param message Parsed purchase request.
 * @param response Response-body storage owned by the caller.
 * @param written Receives the encoded response size.
 * @return True when the refusal was encoded.
 */
[[nodiscard]] bool refuse_purchase(const middleware::web_service::Message& message,
                                   std::span<std::byte> response,
                                   std::size_t& written) noexcept {
    namespace purchase_codec = middleware::web_service::messages::opcode901;
    purchase_codec::Request purchase;
    const bool parsed = purchase_codec::parse_request(message, purchase);
    // The clock verdict is logged, never acted on. Nothing can pass while the route refuses.
    const auto policy = purchase_codec::check_clock(purchase, server_clock_seconds());
    std::array<char, kPurchaseLineCapacity> line{};
    const int length =
        parsed ? std::snprintf(
                     line.data(),
                     line.size(),
                     "ev=ws901 stage=purchase result=refuse vendor=%d sale=%d present=%u policy=%s",
                     static_cast<int>(purchase.vendorIndex),
                     static_cast<int>(purchase.saleIndex),
                     purchase.hasClock ? 1U : 0U,
                     purchase_codec::clock_policy_name(policy))
               : std::snprintf(line.data(),
                               line.size(),
                               "ev=ws901 stage=purchase result=refuse reason=parse");
    if (length > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::error,
                         {line.data(), static_cast<std::size_t>(length)});
    }
    middleware::web_service::StatusResponse status{};
    status.code = kPurchaseRefusedCode;
    // The trailing bool drives a local action effect on the client, so it stays clear.
    status.trailingBool = false;
    return middleware::web_service::encode_response(
        message,
        middleware::web_service::ResponseShape::statusPairWithBool,
        status,
        response,
        written);
}

/** One line carries the picked id and whether the selection moved. */
constexpr std::size_t kSelectLineCapacity = 96;

/**
 * Records the player's character pick, which arrives nowhere else.
 * A bad or unknown id leaves the selection alone. The reply is the status pair either way. The
 * Family-4 object move follows this call, and the family-zero pair after it.
 * @param message Parsed select-character request.
 * @param outcome Gets the picked key once the selection has moved in State.
 */
void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode504::Request picked;
    if (!middleware::web_service::messages::opcode504::parse_request(message, picked)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws504 stage=parse result=fail");
        return;
    }
    bool changed = false;
    if (!state::set_selected_character(picked.characterSoid, changed)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws504 stage=select result=unknown");
        return;
    }
    outcome.hasSelectedCharacter = true;
    outcome.selectedCharacterSoid = picked.characterSoid;

    std::array<char, kSelectLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=ws504 stage=select result=ok soid=0x%llX changed=%u",
                                      static_cast<unsigned long long>(picked.characterSoid),
                                      static_cast<unsigned>(changed));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Answers a request whose own codec refused with the bare correlated echo.
 * The Client matches on the echoed transaction id. A missing body is worse than a thin one. It
 * under-runs the decoder and takes the BAP connection down.
 * @param message Parsed request whose correlation fields are echoed.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size in bytes.
 * @return True when the echo fits.
 */
bool encode_echo(const middleware::web_service::Message& message,
                 std::span<std::byte> response,
                 std::size_t& written) noexcept {
    std::array<char, kOpcodeLineCapacity> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=ws stage=body result=echo opcode=%u", message.opcode);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    namespace ws = middleware::web_service;
    return ws::encode_response(
        message, ws::ResponseShape::generic, ws::StatusResponse{}, response, written);
}

/**
 * Parses and answers one Web Service request with its whole descriptor layout.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @return False only when the envelope header does not parse.
 */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    Outcome outcome;
    return consume(request, response, written, outcome);
}

/**
 * Parses one request, encodes its response, and publishes checked side effects last.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets a valid family selector only after the response is encoded.
 * @return False only when the envelope header does not parse.
 */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written,
             Outcome& outcome) noexcept {
    written = 0;
    outcome = {};
    middleware::web_service::Message message;
    if (!middleware::web_service::parse_request(request, message)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws stage=parse result=fail");
        return false;
    }
    report_opcode(message.opcode);

    if (message.opcode == middleware::web_service::messages::opcode205::kOpcode) {
        const auto investment = state::investment_snapshot();
        return middleware::web_service::messages::opcode205::encode_response(
                   message, investment, response, written)
               || encode_echo(message, response, written);
    }

    if (message.opcode == middleware::web_service::messages::opcode503::kOpcode) {
        middleware::web_service::messages::opcode503::Request bootstrap;
        const bool parsed =
            middleware::web_service::messages::opcode503::parse_request(message, bootstrap);
        // The request's own key is echoed and adopted. An authored id here costs the ship and the
        // banner.
        if (!bootstrap.hasPrimarySoid) {
            bootstrap.primarySoid = state::account_snapshot().primarySoid;
        }
        const auto investment = state::investment_snapshot();
        if (!parsed
            || !middleware::web_service::messages::opcode503::encode_response(
                message, bootstrap, investment, response, written)) {
            return encode_echo(message, response, written);
        }
        if (bootstrap.hasPrimarySoid && !state::set_primary_soid(bootstrap.primarySoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=ws503 stage=adopt result=fail");
        }
        return true;
    }

    if (message.opcode == middleware::web_service::messages::opcode501::kOpcode) {
        // Returns a SOID family three already publishes. The request body is not parsed.
        const std::uint64_t characterSoid =
            state::account::selected_character_soid(state::account_snapshot());
        return middleware::web_service::messages::opcode501::encode_response(
                   message, characterSoid, response, written)
               || encode_echo(message, response, written);
    }

    // Runs before the shared response-shape path, which would answer the success status.
    if (message.opcode == middleware::web_service::messages::opcode901::kOpcode) {
        return refuse_purchase(message, response, written)
               || encode_echo(message, response, written);
    }

    if (message.opcode == middleware::web_service::messages::opcode601::kOpcode) {
        return middleware::web_service::messages::opcode601::encode_response(
                   message, response, written)
               || encode_echo(message, response, written);
    }

    // A subscribe whose body does not parse is still answered; only the subscription is dropped.
    middleware::queuez::Subscription subscription;
    const bool subscribes =
        message.opcode == middleware::web_service::messages::opcode206::kOpcode
        && middleware::web_service::messages::opcode206::parse_request(message, subscription);

    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    if (!middleware::web_service::encode_response(
            message, shape, middleware::web_service::StatusResponse{}, response, written)) {
        return encode_echo(message, response, written);
    }
    if (subscribes) {
        // Publish the subscription only after its correlated response is complete.
        outcome.hasSubscription = true;
        outcome.subscription = subscription;
        return true;
    }
    if (message.opcode == middleware::web_service::messages::opcode504::kOpcode) {
        // The selection is State, not a response field, so it publishes after the reply encodes.
        select_character(message, outcome);
    }
    return true;
}

} // namespace sunrise::server::web_service
