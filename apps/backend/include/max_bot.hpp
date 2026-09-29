#pragma once

#include "domain.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace maxhelp {

struct BotEvent {
    std::string key, type, user_id, chat_id, command, callback_id;
    std::int64_t timestamp = 0;
    std::optional<bool> activity;
    std::string text, payload, display_name;
};

struct Outbound {
    std::string kind, chat_id, callback_id;
    Json body;
};

std::optional<BotEvent> parse_update(const Json& update);
std::optional<Outbound> response_for(const BotEvent& event,
                                     const std::string& bot_username);
Json event_json(const BotEvent& event);
BotEvent event_from_json(const Json& json);
Json outbound_json(const Outbound& outbound);
Outbound outbound_from_json(const Json& json);
Json bot_message(const std::string& text,const Json& rows);
Json bot_menu(const std::string& bot_username);
Outbound bot_reply(const BotEvent& event,const Json& message);

class MaxApiError : public std::runtime_error {
public:
    int status;
    bool retryable;
    int retry_after_seconds;
    MaxApiError(int status, bool retryable, int retry_after_seconds,
                const std::string& message);
};

struct HttpRequest {
    std::string url, authorization, body;
    std::string ca_file{};
};
struct HttpResult {
    int status = 0;
    std::string body;
    int retry_after_seconds = 0;
};
using MaxTransport = std::function<HttpResult(const HttpRequest&)>;

// Classifies HTTP/API errors without logging upstream bodies or secrets.
void check_max_response(const HttpResult& result);

class MaxClient {
public:
    explicit MaxClient(std::string token,
                       std::string base_url = "https://platform-api2.max.ru",
                       MaxTransport transport = {},
                       std::string ca_file = {});
    // Network timeouts are retryable, but delivery may already have happened.
    // MAX does not provide an idempotency key for POST /messages.
    void send(const Outbound& outbound) const;
private:
    std::string token_, base_url_, ca_file_;
    MaxTransport transport_;
};

} // namespace maxhelp
