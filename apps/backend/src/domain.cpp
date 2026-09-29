#include "domain.hpp"

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace maxhelp {
namespace {

[[noreturn]] void invalid_profile(const std::string& message) {
    throw AppError(400, "INVALID_PROFILE", message);
}

[[noreturn]] void invalid_auth() {
    throw AppError(401, "INIT_DATA_INVALID", "Недействительные данные входа MAX.");
}

std::string hex(const unsigned char* bytes, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return result;
}

std::array<unsigned char, 32> hmac_sha256(std::string_view key,
                                         std::string_view data) {
    if (key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("HMAC key is too large");
    }
    std::array<unsigned char, 32> result{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(data.data()), data.size(),
              result.data(), &length) || length != result.size()) {
        throw std::runtime_error("HMAC-SHA256 failed");
    }
    return result;
}

// Count Unicode scalar values, rejecting overlong encodings, surrogates and NUL.
// PostgreSQL text cannot store NUL, even when it arrived as a JSON escape.
bool unicode_length(std::string_view value, std::size_t& count) {
    count = 0;
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        std::uint32_t point = 0;
        std::size_t width = 0;
        std::uint32_t minimum = 0;
        if (lead > 0 && lead <= 0x7f) {
            point = lead;
            width = 1;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            point = lead & 0x1f;
            width = 2;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            point = lead & 0x0f;
            width = 3;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            point = lead & 0x07;
            width = 4;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (width > value.size() - i) {
            return false;
        }
        for (std::size_t j = 1; j < width; ++j) {
            const auto next = static_cast<unsigned char>(value[i + j]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            point = (point << 6) | (next & 0x3f);
        }
        if (point < minimum || point > 0x10ffff ||
            (point >= 0xd800 && point <= 0xdfff)) {
            return false;
        }
        ++count;
        i += width;
    }
    return true;
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string decode_value(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            // MAX's documented decodeURIComponent keeps a literal '+' intact.
            decoded.push_back(value[i]);
            continue;
        }
        if (value.size() - i < 3) invalid_auth();
        const auto high = hex_digit(value[i + 1]);
        const auto low = hex_digit(value[i + 2]);
        if (high < 0 || low < 0) invalid_auth();
        decoded.push_back(static_cast<char>(high * 16 + low));
        i += 2;
    }
    std::size_t count = 0;
    if (!unicode_length(decoded, count)) invalid_auth();
    return decoded;
}

bool identifier_character(char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

std::map<std::string, std::string> parse_parameters(const std::string& raw) {
    if (raw.empty() || raw.size() > 65536) invalid_auth();
    std::map<std::string, std::string> result;
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const auto end = raw.find('&', offset);
        const auto size = (end == std::string::npos ? raw.size() : end) - offset;
        const std::string_view pair(raw.data() + offset, size);
        const auto separator = pair.find('=');
        if (separator == std::string_view::npos || separator == 0) invalid_auth();
        const std::string key(pair.substr(0, separator));
        // Keys are not URL-decoded by the MAX signing algorithm. Restricting
        // them to identifiers also rejects ambiguous encoded aliases of keys.
        if (key.size() > 128 ||
            !std::all_of(key.begin(), key.end(), identifier_character)) {
            invalid_auth();
        }
        const auto value = decode_value(pair.substr(separator + 1));
        if (!result.emplace(key, value).second) invalid_auth();
        if (end == std::string::npos) break;
        offset = end + 1;
        if (offset == raw.size()) invalid_auth();
    }
    return result;
}

void require_fields(const Json& object,
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional = {}) {
    if (!object.is_object()) invalid_profile("Ожидается JSON-объект.");
    for (const auto key : required) {
        if (!object.contains(std::string(key))) {
            invalid_profile("Отсутствует поле: " + std::string(key));
        }
    }
    for (auto item = object.begin(); item != object.end(); ++item) {
        const auto& key = item.key();
        const auto allowed = std::find(required.begin(), required.end(), key) != required.end() ||
                             std::find(optional.begin(), optional.end(), key) != optional.end();
        if (!allowed || item.value().is_null()) {
            invalid_profile("Недопустимое поле или значение: " + key);
        }
    }
}

std::string text_field(const Json& object, const char* key,
                       std::size_t minimum, std::size_t maximum) {
    const auto& value = object.at(key);
    if (!value.is_string()) invalid_profile(std::string(key) + ": ожидается строка.");
    const auto& result = value.get_ref<const std::string&>();
    std::size_t count = 0;
    if (!unicode_length(result, count) || count < minimum || count > maximum) {
        invalid_profile(std::string(key) + ": недопустимая длина или кодировка.");
    }
    return result;
}

bool canonical_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool topic_matches(const std::string& topic, const std::string& prefix) {
    return prefix == "*" || topic == prefix ||
           (topic.size() > prefix.size() && topic.starts_with(prefix) &&
            topic[prefix.size()] == '.');
}

bool valid_evidence_url(const std::string& value) {
    if (value.empty() || value.size() > 8192 || value.find('\\') != std::string::npos) {
        return false;
    }
    for (const auto byte : value) {
        if (static_cast<unsigned char>(byte) <= 0x20 ||
            static_cast<unsigned char>(byte) == 0x7f) return false;
    }
    using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
    Url url(curl_url(), &curl_url_cleanup);
    if (!url) throw std::runtime_error("Could not allocate URL parser");
    // CURLU_DISALLOW_USER rejects even empty credentials. No network I/O occurs.
    if (curl_url_set(url.get(), CURLUPART_URL, value.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) {
        return false;
    }
    const auto part = [&url](CURLUPart field) {
        char* raw = nullptr;
        const auto status = curl_url_get(url.get(), field, &raw, 0);
        std::unique_ptr<char, decltype(&curl_free)> owner(raw, &curl_free);
        return status == CURLUE_OK ? std::string(raw) : std::string();
    };
    const auto scheme = part(CURLUPART_SCHEME);
    const auto host = part(CURLUPART_HOST);
    return scheme == "https" && !host.empty();
}

void validate_facets(const Json& selected, const std::string& topic,
                     const Json& definitions) {
    if (!selected.is_object()) invalid_profile("facets: ожидается объект.");
    for (auto item = selected.begin(); item != selected.end(); ++item) {
        const Json* definition = nullptr;
        for (const auto& candidate : definitions) {
            if (candidate.at("id") == item.key()) {
                definition = &candidate;
                break;
            }
        }
        if (!definition) invalid_profile("Неизвестный признак: " + item.key());
        bool applicable = false;
        for (const auto& prefix : definition->at("applicable_topic_prefixes")) {
            if (topic_matches(topic, prefix.get<std::string>())) applicable = true;
        }
        if (!applicable) invalid_profile("Признак неприменим к выбранной теме.");
        if (!item.value().is_array() || item.value().empty()) {
            invalid_profile("Значения признака должны быть непустым массивом.");
        }
        std::set<std::string> seen;
        for (const auto& choice : item.value()) {
            if (!choice.is_string()) invalid_profile("Значение признака должно быть строкой.");
            const auto& choice_id = choice.get_ref<const std::string&>();
            const auto& values = definition->at("values");
            const auto known = std::any_of(values.begin(), values.end(), [&choice_id](const Json& value) {
                return value.at("id") == choice_id;
            });
            if (!known || !seen.insert(choice_id).second) {
                invalid_profile("Неизвестное или повторяющееся значение признака.");
            }
        }
    }
}

} // namespace

AppError::AppError(int status_value, std::string code_value, std::string message)
    : std::runtime_error(std::move(message)), status(status_value), code(std::move(code_value)) {}

Json parse_json_strict(const std::string& raw) {
    std::vector<std::set<std::string>> objects;
    auto callback = [&objects](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 64) throw AppError(400, "INVALID_JSON", "JSON слишком глубоко вложен.");
        if (event == Json::parse_event_t::object_start) {
            objects.emplace_back();
        } else if (event == Json::parse_event_t::key) {
            if (objects.empty() || !objects.back().insert(parsed.get<std::string>()).second) {
                throw AppError(400, "INVALID_JSON", "Повторяющееся поле JSON.");
            }
        } else if (event == Json::parse_event_t::object_end) {
            objects.pop_back();
        }
        return true;
    };
    try {
        return Json::parse(raw, callback);
    } catch (const Json::exception&) {
        throw AppError(400, "INVALID_JSON", "Некорректный JSON.");
    }
}

std::string sha256(const std::string& value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("SHA256 failed");
    }
    return hex(digest.data(), length);
}

std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("Secure token generation failed");
    }
    return hex(bytes.data(), bytes.size());
}

std::string uuid() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("UUID generation failed");
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const auto value = hex(bytes.data(), bytes.size());
    return value.substr(0, 8) + "-" + value.substr(8, 4) + "-" + value.substr(12, 4) +
           "-" + value.substr(16, 4) + "-" + value.substr(20, 12);
}

bool constant_equals(const std::string& left, const std::string& right) {
    return left.size() == right.size() &&
           CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

Json verify_init_data(const std::string& raw, const std::string& token,
                      int max_age, long long now_seconds) {
    if (token.empty()) {
        throw AppError(503, "MAX_NOT_CONFIGURED", "Вход через MAX пока не настроен.");
    }
    if (max_age <= 0 || now_seconds < 0) throw std::invalid_argument("Invalid auth clock or max age");
    auto parameters = parse_parameters(raw);
    if (!parameters.contains("hash") || !parameters.contains("auth_date") ||
        !parameters.contains("user")) invalid_auth();
    const auto signature = parameters.at("hash");
    if (signature.size() != 64 ||
        !std::all_of(signature.begin(), signature.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) invalid_auth();
    parameters.erase("hash");
    std::string launch_parameters;
    for (const auto& [key, value] : parameters) {
        if (!launch_parameters.empty()) launch_parameters += '\n';
        launch_parameters += key + "=" + value;
    }
    const auto secret = hmac_sha256("WebAppData", token);
    const std::string_view secret_view(reinterpret_cast<const char*>(secret.data()), secret.size());
    const auto expected = hmac_sha256(secret_view, launch_parameters);
    if (!constant_equals(hex(expected.data(), expected.size()), signature)) invalid_auth();

    const auto& date_text = parameters.at("auth_date");
    long long auth_date = 0;
    const auto conversion = std::from_chars(date_text.data(), date_text.data() + date_text.size(), auth_date);
    if (date_text.empty() || !std::all_of(date_text.begin(), date_text.end(), [](char value) {
            return value >= '0' && value <= '9';
        }) || conversion.ec != std::errc{} ||
        conversion.ptr != date_text.data() + date_text.size() || auth_date <= 0) {
        invalid_auth();
    }
    if ((auth_date > now_seconds && auth_date - now_seconds > 30) ||
        (auth_date <= now_seconds && now_seconds - auth_date > max_age)) {
        throw AppError(401, "INIT_DATA_EXPIRED", "Данные входа MAX устарели. Откройте приложение заново.");
    }

    Json user;
    try {
        user = parse_json_strict(parameters.at("user"));
    } catch (const AppError&) {
        invalid_auth();
    }
    if (!user.is_object() || !user.contains("id") || !user.at("id").is_number_integer()) invalid_auth();
    // nlohmann marks unsigned integers as integers, so test the upper bound
    // before conversion: get<int64_t>() would silently wrap large uint64_t.
    if (user.at("id").is_number_unsigned() &&
        user.at("id").get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        invalid_auth();
    }
    const auto id = user.at("id").get<std::int64_t>();
    if (id <= 0) invalid_auth();
    std::string name;
    for (const auto* field : {"first_name", "last_name"}) {
        if (!user.contains(field) || user.at(field).is_null()) continue;
        if (!user.at(field).is_string()) invalid_auth();
        const auto& part = user.at(field).get_ref<const std::string&>();
        std::size_t count = 0;
        if (!unicode_length(part, count)) invalid_auth();
        if (!part.empty()) {
            if (!name.empty()) name += ' ';
            name += part;
        }
    }
    if (name.empty()) name = "Пользователь MAX";
    // Keep platform names within the profile's 100-character database limit.
    std::size_t characters = 0;
    std::size_t cut = 0;
    while (cut < name.size() && characters < 100) {
        ++cut;
        while (cut < name.size() && (static_cast<unsigned char>(name[cut]) & 0xc0) == 0x80) ++cut;
        ++characters;
    }
    name.resize(cut);
    return Json{{"id", std::to_string(id)}, {"displayName", name}};
}

Json validate_profile(const Json& input, const Json& taxonomy) {
    require_fields(input, {"displayName", "bio", "availableToHelp", "maxActiveConversations", "competencies"});
    text_field(input, "displayName", 1, 100);
    text_field(input, "bio", 0, 500);
    if (!input.at("availableToHelp").is_boolean()) invalid_profile("availableToHelp: ожидается boolean.");
    const auto& capacity = input.at("maxActiveConversations");
    if (!capacity.is_number_integer() || capacity < 1 || capacity > 5) {
        invalid_profile("maxActiveConversations: допустимо целое число от 1 до 5.");
    }
    const auto& competencies = input.at("competencies");
    if (!competencies.is_array() || competencies.size() > 30) {
        invalid_profile("Допустимо не более 30 компетенций.");
    }
    std::set<std::string> ids;
    for (const auto& competency : competencies) {
        require_fields(competency, {"topicId", "facets", "experienceKind", "description", "evidenceVisibility"},
                       {"id", "evidenceUrl"});
        if (competency.contains("id")) {
            const auto id = text_field(competency, "id", 36, 36);
            if (!canonical_uuid(id) || !ids.insert(id).second) {
                invalid_profile("Недопустимый или повторяющийся id компетенции.");
            }
        }
        const auto topic_id = text_field(competency, "topicId", 1, 200);
        const auto& topics = taxonomy.at("topics");
        const auto topic = std::find_if(topics.begin(), topics.end(), [&topic_id](const Json& value) {
            return value.at("id") == topic_id;
        });
        if (topic == topics.end() || !topic->at("active").get<bool>() || topic->at("level") < 2) {
            invalid_profile("Выберите действующую тему уровня L2 или глубже.");
        }
        validate_facets(competency.at("facets"), topic_id, taxonomy.at("facets"));
        const auto experience = text_field(competency, "experienceKind", 1, 30);
        if (experience != "self_study" && experience != "practice" &&
            experience != "teaching" && experience != "participation") {
            invalid_profile("Неизвестный вид опыта.");
        }
        text_field(competency, "description", 0, 500);
        const auto visibility = text_field(competency, "evidenceVisibility", 1, 20);
        if (visibility != "private" && visibility != "participants") {
            invalid_profile("Недопустимая видимость свидетельства.");
        }
        if (competency.contains("evidenceUrl")) {
            const auto url = text_field(competency, "evidenceUrl", 1, 8192);
            if (!valid_evidence_url(url)) {
                invalid_profile("Ссылка должна быть корректным HTTPS URL без имени пользователя и пароля.");
            }
        }
    }
    return input;
}

} // namespace maxhelp
