#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace maxhelp {

using Json = nlohmann::json;

class AppError : public std::runtime_error {
public:
    int status;
    std::string code;
    AppError(int status, std::string code, std::string message);
};

// Verifies the original window.WebApp.initData using the server-only bot token.
Json verify_init_data(const std::string& raw, const std::string& token,
                      int max_age, long long now_seconds);

// Returns the validated writable fields. Ownership and server fields are handled
// by the repository: an optional competency id identifies an existing entry.
Json validate_profile(const Json& input, const Json& taxonomy);

// Rejects malformed UTF-8/JSON and duplicate keys at every object depth.
Json parse_json_strict(const std::string& raw);

std::string sha256(const std::string& value);
std::string random_token();
std::string uuid();
bool constant_equals(const std::string& left, const std::string& right);

} // namespace maxhelp
