#include "domain.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using maxhelp::Json;

namespace {

int checks = 0;
constexpr long long now = 1771409719;
const std::string test_token = "local-test-token-not-a-real-bot-token";

void check(bool condition, const std::string& description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

void rejects(const std::function<void()>& operation, int status, const std::string& description) {
    ++checks;
    try {
        operation();
    } catch (const maxhelp::AppError& error) {
        if (error.status == status) return;
        throw std::runtime_error(description + ": wrong HTTP status " + std::to_string(error.status));
    }
    throw std::runtime_error(description + ": request was accepted");
}

std::string percent_encode(const std::string& input) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char byte : input) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.') {
            result += static_cast<char>(byte);
        } else {
            result += '%';
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
    }
    return result;
}

std::vector<unsigned char> sign(const std::string& key, const std::string& value) {
    std::vector<unsigned char> result(EVP_MAX_MD_SIZE);
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(value.data()), value.size(),
              result.data(), &length)) {
        throw std::runtime_error("Test HMAC failed");
    }
    result.resize(length);
    return result;
}

std::string signed_parameters(const std::map<std::string, std::string>& parameters) {
    std::string message;
    std::string raw;
    for (const auto& [key, value] : parameters) {
        if (!message.empty()) message += '\n';
        message += key + "=" + value;
        if (!raw.empty()) raw += '&';
        raw += key + "=" + percent_encode(value);
    }
    const auto secret = sign("WebAppData", test_token);
    const auto signature = sign(std::string(secret.begin(), secret.end()), message);
    constexpr char hex[] = "0123456789abcdef";
    raw += "&hash=";
    for (auto byte : signature) {
        raw += hex[byte >> 4];
        raw += hex[byte & 15];
    }
    return raw;
}

std::string auth_fixture(const Json& user = Json{{"id", 123}, {"first_name", "Анна"}, {"last_name", "Петрова"}},
                         long long date = now) {
    return signed_parameters({{"auth_date", std::to_string(date)}, {"user", user.dump()}});
}

Json taxonomy() {
    return Json{
        {"topics", Json::array({
            {{"id", "science"}, {"active", true}, {"level", 1}},
            {{"id", "science.math"}, {"active", true}, {"level", 2}},
            {{"id", "science.math.percentages"}, {"active", true}, {"level", 3}},
            {{"id", "science.maths"}, {"active", true}, {"level", 2}},
            {{"id", "science.physics"}, {"active", false}, {"level", 2}},
            {{"id", "digital.spreadsheets"}, {"active", true}, {"level", 2}}
        })},
        {"facets", Json::array({
            {{"id", "learner_level"}, {"applicable_topic_prefixes", Json::array({"*"})},
             {"values", Json::array({{{"id", "beginner"}}, {{"id", "basics"}}})}},
            {{"id", "math_context"}, {"applicable_topic_prefixes", Json::array({"science.math"})},
             {"values", Json::array({{{"id", "school"}}})}},
            {{"id", "tool"}, {"applicable_topic_prefixes", Json::array({"digital.spreadsheets"})},
             {"values", Json::array({{{"id", "excel"}}, {{"id", "google_sheets"}}})}}
        })}
    };
}

Json profile() {
    return Json{
        {"displayName", "Анна"}, {"bio", "Могу объяснить проценты."},
        {"availableToHelp", true}, {"maxActiveConversations", 2},
        {"competencies", Json::array({
            {{"topicId", "science.math.percentages"},
             {"facets", {{"learner_level", Json::array({"beginner"})},
                         {"math_context", Json::array({"school"})}}},
             {"experienceKind", "practice"}, {"description", "Объясняю на примерах."},
             {"evidenceVisibility", "private"}}
        })}
    };
}

std::string repeat(const std::string& value, int count) {
    std::string result;
    for (int i = 0; i < count; ++i) result += value;
    return result;
}

void test_crypto() {
    check(maxhelp::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 vector");
    check(maxhelp::constant_equals("value", "value"), "equal strings");
    check(!maxhelp::constant_equals("value", "VALUE"), "case-sensitive comparison");
    check(!maxhelp::constant_equals("value", "value-more"), "different lengths");
    check(maxhelp::constant_equals("", ""), "empty comparison");
    const auto token = maxhelp::random_token();
    check(token.size() == 64 && token != maxhelp::random_token(), "independent 256-bit opaque tokens");
    const auto id = maxhelp::uuid();
    check(id.size() == 36 && id[14] == '4' && std::string("89ab").find(id[19]) != std::string::npos,
          "UUID v4 version and variant");
}

void test_json() {
    check(maxhelp::parse_json_strict(R"({"a":[{"x":1},{"x":2}],"b":true})").at("b") == true, "nested JSON accepted");
    for (const auto& bad : std::vector<std::string>{
            R"({"x":1,"x":2})", R"({"nested":{"x":1,"x":2}})",
            R"({"x":1,"\u0078":2})", R"([{"x":1,"x":2}])",
            R"({"a":1,})", R"({"a":1}false)", R"({"a":"\ud800"})", ""}) {
        rejects([&] { maxhelp::parse_json_strict(bad); }, 400, "malformed or duplicate-key JSON");
    }
    const std::string invalid_utf8 = std::string("{\"a\":\"") + char(0xc0) + char(0xaf) + "\"}";
    rejects([&] { maxhelp::parse_json_strict(invalid_utf8); }, 400, "invalid UTF8 JSON");
    const auto deep = repeat("[", 70) + "0" + repeat("]", 70);
    rejects([&] { maxhelp::parse_json_strict(deep); }, 400, "excessive JSON depth");
}

void test_auth() {
    const auto good = auth_fixture();
    const auto user = maxhelp::verify_init_data(good, test_token, 3600, now);
    check(user.at("id") == "123" && user.at("displayName") == "Анна Петрова", "auth returns verified identity");
    rejects([&] { maxhelp::verify_init_data(good, "wrong-server-token", 3600, now); }, 401, "wrong signing key");
    rejects([&] { maxhelp::verify_init_data(good, "", 3600, now); }, 503, "unconfigured MAX");
    auto forged = good;
    forged.back() = forged.back() == '0' ? '1' : '0';
    rejects([&] { maxhelp::verify_init_data(forged, test_token, 3600, now); }, 401, "forged signature");
    for (const auto suffix : {"&user=%7B%7D", "&auth_date=1", "&hash=0", "&%68ash=0", "&", "&bad=%ZZ"}) {
        rejects([&] { maxhelp::verify_init_data(good + suffix, test_token, 3600, now); }, 401, "duplicate or malformed URL parameter");
    }
    const auto duplicate_extra = signed_parameters({{"auth_date", std::to_string(now)}, {"query_id", "one"}, {"user", R"({"id":1})"}}) + "&query_id=two";
    rejects([&] { maxhelp::verify_init_data(duplicate_extra, test_token, 3600, now); }, 401, "duplicate additional parameter");
    rejects([&] { maxhelp::verify_init_data(auth_fixture(Json{{"id", 1}}, now - 3601), test_token, 3600, now); }, 401, "expired signature");
    rejects([&] { maxhelp::verify_init_data(auth_fixture(Json{{"id", 1}}, now + 31), test_token, 3600, now); }, 401, "future signature");
    check(maxhelp::verify_init_data(auth_fixture(Json{{"id", 1}}, now + 30), test_token, 3600, now).at("id") == "1", "future clock tolerance boundary");
    check(maxhelp::verify_init_data(auth_fixture(Json{{"id", 1}}, now - 3600), test_token, 3600, now).at("id") == "1", "expiry boundary");
    const auto max_id = std::numeric_limits<std::int64_t>::max();
    check(maxhelp::verify_init_data(auth_fixture(Json{{"id", max_id}}), test_token, 3600, now).at("id") == std::to_string(max_id), "int64 identity preserved");
    for (const auto& id : std::vector<Json>{0, -1, "123", true, 1.5, Json(), std::uint64_t{9223372036854775808ULL}, std::numeric_limits<std::uint64_t>::max()}) {
        rejects([&] { maxhelp::verify_init_data(auth_fixture(Json{{"id", id}}), test_token, 3600, now); }, 401, "invalid or overflowing user id");
    }
    const auto duplicate_user = signed_parameters({{"auth_date", std::to_string(now)}, {"user", R"({"id":1,"id":2})"}});
    rejects([&] { maxhelp::verify_init_data(duplicate_user, test_token, 3600, now); }, 401, "duplicate signed JSON identity");
    for (const auto& bad_date : {"", "-1", "+1771409719", "1771409719x", "9223372036854775808"}) {
        const auto raw = signed_parameters({{"auth_date", bad_date}, {"user", R"({"id":1})"}});
        rejects([&] { maxhelp::verify_init_data(raw, test_token, 3600, now); }, 401, "malformed signed timestamp");
    }
    auto name = Json{{"id", 1}, {"first_name", "A+B %26 C=D"}};
    check(maxhelp::verify_init_data(auth_fixture(name), test_token, 3600, now).at("displayName") == "A+B %26 C=D", "decode values exactly once");
    const auto literal_plus = signed_parameters({{"auth_date", std::to_string(now)}, {"start_param", "+"}, {"user", R"({"id":1})"}});
    auto plus_raw = literal_plus;
    plus_raw.replace(plus_raw.find("%2B"), 3, "+");
    check(maxhelp::verify_init_data(plus_raw, test_token, 3600, now).at("id") == "1", "MAX literal plus semantics");
    name["first_name"] = repeat("🙂", 101);
    check(maxhelp::verify_init_data(auth_fixture(name), test_token, 3600, now).at("displayName") == repeat("🙂", 100), "long MAX name truncates at Unicode boundary");
    auto upper_signature = good;
    const auto hash_offset = upper_signature.find("&hash=") + 6;
    std::transform(upper_signature.begin() + static_cast<std::ptrdiff_t>(hash_offset), upper_signature.end(), upper_signature.begin() + static_cast<std::ptrdiff_t>(hash_offset), [](char value) {
        return value >= 'a' && value <= 'f' ? static_cast<char>(value - 'a' + 'A') : value;
    });
    rejects([&] { maxhelp::verify_init_data(upper_signature, test_token, 3600, now); }, 401, "exact lowercase signature comparison");
}

void test_profile() {
    const auto catalog = taxonomy();
    auto input = profile();
    check(maxhelp::validate_profile(input, catalog) == input, "valid profile unchanged");
    input["competencies"] = Json::array();
    check(maxhelp::validate_profile(input, catalog).at("competencies").empty(), "empty competencies allowed");
    const auto reject_edit = [&](const std::function<void(Json&)>& edit, const std::string& label) {
        auto candidate = profile();
        edit(candidate);
        rejects([&] { maxhelp::validate_profile(candidate, catalog); }, 400, label);
    };
    for (const auto* field : {"id", "provenance", "unknown", "verified"}) {
        reject_edit([&](Json& value) { value[field] = "forged"; }, "readonly or unknown profile field");
    }
    for (const auto* field : {"provenance", "evidenceStatus", "verified", "extra"}) {
        reject_edit([&](Json& value) { value["competencies"][0][field] = "verified"; }, "readonly or unknown competency field");
    }
    for (const auto* field : {"displayName", "bio", "availableToHelp", "maxActiveConversations", "competencies"}) {
        reject_edit([&](Json& value) { value.erase(field); }, "required profile field");
        reject_edit([&](Json& value) { value[field] = nullptr; }, "null profile field");
    }
    reject_edit([](Json& value) { value["displayName"] = ""; }, "empty display name");
    reject_edit([](Json& value) { value["displayName"] = repeat("🙂", 101); }, "101 Unicode scalar values");
    input = profile();
    input["displayName"] = repeat("🙂", 100);
    input["bio"] = repeat("я", 500);
    input["competencies"][0]["description"] = repeat("🙂", 500);
    check(maxhelp::validate_profile(input, catalog) == input, "Unicode lengths count characters not bytes");
    reject_edit([](Json& value) { value["bio"] = repeat("я", 501); }, "bio too long");
    reject_edit([](Json& value) { value["competencies"][0]["description"] = repeat("🙂", 501); }, "description too long");
    reject_edit([](Json& value) { value["displayName"] = std::string("bad") + char(0xc0) + char(0xaf); }, "invalid UTF8 value");
    reject_edit([](Json& value) { value["bio"] = std::string("a\0b", 3); }, "NUL rejected before database");
    for (const auto& capacity : std::vector<Json>{0, 6, 2.0, "2", true, std::numeric_limits<std::uint64_t>::max()}) {
        reject_edit([&](Json& value) { value["maxActiveConversations"] = capacity; }, "strict bounded integer capacity");
    }
    reject_edit([](Json& value) { value["availableToHelp"] = "true"; }, "strict boolean");
    reject_edit([](Json& value) { value["competencies"] = std::vector<Json>(31, value["competencies"][0]); }, "31 competencies");
    for (const auto* topic : {"science", "science.physics", "unknown", "science.maths"}) {
        reject_edit([&](Json& value) { value["competencies"][0]["topicId"] = topic; }, "inactive, unknown, L1 or prefix-boundary topic");
    }
    reject_edit([](Json& value) { value["competencies"][0]["facets"]["unknown"] = Json::array({"beginner"}); }, "unknown facet");
    reject_edit([](Json& value) { value["competencies"][0]["facets"]["tool"] = Json::array({"excel"}); }, "facet cannot borrow applicability from another topic");
    for (const auto& bad_choice : std::vector<Json>{"beginner", Json::array(), Json::array({"bogus"}), Json::array({nullptr}), Json::array({"beginner", "beginner"})}) {
        reject_edit([&](Json& value) { value["competencies"][0]["facets"]["learner_level"] = bad_choice; }, "invalid facet selection");
    }
    reject_edit([](Json& value) { value["competencies"][0]["experienceKind"] = "expert"; }, "unknown experience");
    reject_edit([](Json& value) { value["competencies"][0]["evidenceVisibility"] = "public"; }, "unknown evidence visibility");
    for (const auto* url : {"", "http://example.org", "https://user:secret@example.org", "https://@example.org", "https://example.org:65536", "https://example.org:abc", "https://example.org:-1", "https:///", "https://example.org/path with space", "https://example.org\\evil", "javascript:alert(1)"}) {
        reject_edit([&](Json& value) { value["competencies"][0]["evidenceUrl"] = url; }, "invalid evidence URL");
    }
    for (const auto* url : {"https://example.org/proof?item=1#result", "https://example.org:8443/proof", "https://[2001:db8::1]/proof"}) {
        input = profile();
        input["competencies"][0]["evidenceUrl"] = url;
        check(maxhelp::validate_profile(input, catalog) == input, "valid HTTPS URL accepted");
    }
    reject_edit([](Json& value) { value["competencies"][0]["evidenceUrl"] = nullptr; }, "optional evidence URL cannot be null");
    input = profile();
    input["competencies"][0]["id"] = "78ec0798-222a-41a8-bfef-2113b35ca54a";
    check(maxhelp::validate_profile(input, catalog) == input, "existing competency identifier accepted for ownership check");
    reject_edit([](Json& value) { value["competencies"][0]["id"] = "bad-id"; }, "malformed competency id");
    input["competencies"].push_back(input["competencies"][0]);
    rejects([&] { maxhelp::validate_profile(input, catalog); }, 400, "duplicate competency id");
}

} // namespace

int main() {
    try {
        test_crypto();
        test_json();
        test_auth();
        test_profile();
        std::cout << "Passed " << checks << " domain checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Domain test failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
