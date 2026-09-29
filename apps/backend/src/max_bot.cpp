#include "max_bot.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <ctime>
#include <limits>
#include <memory>
#include <regex>
#include <string_view>
#include <utility>

namespace maxhelp {
namespace {
constexpr std::size_t response_limit = 1024 * 1024;

[[noreturn]] void invalid_update() {
    throw AppError(400, "invalid_max_update", "Некорректное событие MAX");
}
const Json& object_field(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_object()) invalid_update();
    return object.at(key);
}
std::string string_field(const Json& object, const char* key, std::size_t limit = 1024) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) invalid_update();
    auto value = object.at(key).get<std::string>();
    if (value.empty() || value.size() > limit) invalid_update();
    return value;
}
std::int64_t integer_field(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) invalid_update();
    const auto& value = object.at(key);
    if (!value.is_number_integer()) invalid_update();
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) invalid_update();
    return value.get<std::int64_t>();
}
std::string identity(const Json& object, const char* key) {
    const auto value = integer_field(object, key);
    if (value == 0) invalid_update();
    return std::to_string(value);
}
bool human(const Json& user) {
    if (!user.contains("is_bot") || !user.at("is_bot").is_boolean()) invalid_update();
    return !user.at("is_bot").get<bool>();
}
std::string command_from_text(const Json& body) {
    if (!body.contains("text") || body.at("text").is_null()) return "unknown";
    if (!body.at("text").is_string()) invalid_update();
    const auto& text = body.at("text").get_ref<const std::string&>();
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "unknown";
    const auto end = text.find_first_of(" \t\r\n", start);
    const auto token = text.substr(start, end == std::string::npos ? end : end - start);
    if (token == "/start") return "start";
    if (token == "/help") return "help";
    if (token == "/rules") return "rules";
    if (token == "/menu") return "start";
    if (token == "/ask") return "ask";
    if (token == "/knowledge") return "knowledge";
    if (token == "/stats") return "stats";
    if (token == "/requests") return "requests";
    if (token == "/cancel") return "cancel";
    if (token == "/back") return "back";
    if (token == "/resume") return "resume";
    if (!token.starts_with('/')) return "text";
    return "unknown";
}
Json message_body(const std::string& username, bool help) {
    return bot_message(help
        ? "Синапс помогает найти человека с опытом вашего образовательного или карьерного перехода. Два направления: образовательные траектории и карьера. Задайте вопрос прямо здесь: заголовок, описание, тема, необязательные уточнения и цель. Сначала сохраните черновик, затем опубликуйте его. Чтобы помогать, добавьте знание и выберите готовность помогать.\n\n/ask — вопрос\n/knowledge — знание\n/requests — мои вопросы и отклики\n/stats — статистика\n/resume — продолжить ввод\n/back — предыдущий шаг\n/cancel — отменить ввод\n/menu — меню\n\nОтклики, выбор помощника и переписка доступны в mini-app этого же бота."
        : "Добро пожаловать в Синапс! Выбираете программу обучения, готовитесь к стажировке или новому этапу работы? Задайте вопрос человеку с похожим опытом или расскажите, с чем можете помочь сами. Направления: образование и карьера. Выберите действие. Незавершённый ввод доступен через /resume.",bot_menu(username));
}
struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw MaxApiError(0, false, 0, "MAX transport initialization failed");
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};
using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using CurlHeaders = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
struct ResponseBuffer {
    std::string body;
    int retry_after_seconds = 0;
    std::size_t header_bytes = 0;
    bool overflow = false;
};
std::size_t receive_body(char* bytes, std::size_t size, std::size_t count, void* state) noexcept {
    auto& buffer = *static_cast<ResponseBuffer*>(state);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto length = size * count;
    if (length > response_limit - buffer.body.size()) { buffer.overflow = true; return 0; }
    try { buffer.body.append(bytes, length); } catch (...) { return 0; }
    return length;
}
std::size_t receive_header(char* bytes, std::size_t size, std::size_t count, void* state) noexcept {
    auto& buffer = *static_cast<ResponseBuffer*>(state);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto length = size * count;
    if (length > 64 * 1024 - buffer.header_bytes) { buffer.overflow = true; return 0; }
    buffer.header_bytes += length;
    try {
        std::string line(bytes, length);
        const auto colon = line.find(':');
        if (colon == std::string::npos) return length;
        auto name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name != "retry-after") return length;
        auto value = line.substr(colon + 1);
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return length;
        value = value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
        long long seconds = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            const auto date = curl_getdate(value.c_str(), nullptr);
            if (date < 0) return length;
            seconds = static_cast<long long>(date) - static_cast<long long>(std::time(nullptr));
        }
        buffer.retry_after_seconds = static_cast<int>(std::clamp(seconds, 0LL, static_cast<long long>(INT_MAX)));
    } catch (...) { return 0; }
    return length;
}
HttpResult curl_send(const HttpRequest& request) {
    static const CurlGlobal global;
    CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
    if (!handle) throw MaxApiError(0, false, 0, "MAX transport initialization failed");
    curl_slist* raw_headers = nullptr;
    for (const auto& value : {std::string("Authorization: ") + request.authorization,
                             std::string("Content-Type: application/json"), std::string("Accept: application/json")}) {
        auto* appended = curl_slist_append(raw_headers, value.c_str());
        if (!appended) { curl_slist_free_all(raw_headers); throw MaxApiError(0, false, 0, "MAX transport initialization failed"); }
        raw_headers = appended;
    }
    CurlHeaders headers(raw_headers, &curl_slist_free_all);
    ResponseBuffer buffer;
    const auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(handle.get(), option, value) != CURLE_OK)
            throw MaxApiError(0, false, 0, "MAX transport configuration failed");
    };
    set(CURLOPT_URL, request.url.c_str());
    set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_POST, 1L);
    set(CURLOPT_POSTFIELDS, request.body.c_str());
    set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    set(CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    set(CURLOPT_TIMEOUT_MS, 10000L);
    set(CURLOPT_NOSIGNAL, 1L);
    if (!request.ca_file.empty()) set(CURLOPT_CAINFO, request.ca_file.c_str());
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    set(CURLOPT_PROTOCOLS_STR, "https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    set(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    set(CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    set(CURLOPT_WRITEFUNCTION, &receive_body);
    set(CURLOPT_WRITEDATA, &buffer);
    set(CURLOPT_HEADERFUNCTION, &receive_header);
    set(CURLOPT_HEADERDATA, &buffer);
    const auto code = curl_easy_perform(handle.get());
    if (code != CURLE_OK) {
        // A timeout/send/receive failure may happen after MAX accepted the POST.
        const bool retryable = code == CURLE_OPERATION_TIMEDOUT || code == CURLE_COULDNT_CONNECT
            || code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_RESOLVE_PROXY
            || code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR || code == CURLE_GOT_NOTHING;
        throw MaxApiError(0, retryable && !buffer.overflow, buffer.retry_after_seconds,
                          buffer.overflow ? "MAX response exceeded size limit" : "MAX transport failed");
    }
    long status = 0;
    if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK)
        throw MaxApiError(0, false, 0, "MAX response status unavailable");
    return {static_cast<int>(status), std::move(buffer.body), buffer.retry_after_seconds};
}
std::string url_encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += static_cast<char>(c);
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    return encoded;
}
} // namespace

Json bot_message(const std::string& text,const Json& rows) {
    return {{"text",text},{"notify",false},{"attachments",Json::array({Json{{"type","inline_keyboard"},{"payload",{{"buttons",rows}}}}})}};
}
Json bot_menu(const std::string& username) {
    Json rows=Json::array();
    if(!username.empty())rows.push_back(Json::array({Json{{"type","open_app"},{"text","Открыть Синапс"},{"web_app",username},{"payload","profile"}}}));
    for(const auto& [label,command]:std::initializer_list<std::pair<std::string,std::string>>{
        {"Задать вопрос","ask"},{"Добавить знание","knowledge"},{"Мои вопросы и отклики","requests"},
        {"Статистика","stats"},{"Продолжить ввод","resume"},{"Как это работает","help"},{"Правила сообщества","rules"}})
        rows.push_back(Json::array({Json{{"type","callback"},{"text",label},{"payload",command}}}));
    return rows;
}
Outbound bot_reply(const BotEvent& event,const Json& message) {
    if(event.type=="message_callback")return {"answer",event.chat_id,event.callback_id,Json{{"message",message}}};
    return {"message",event.chat_id,"",message};
}

std::optional<BotEvent> parse_update(const Json& update) {
    if (!update.is_object()) invalid_update();
    BotEvent event;
    event.type = string_field(update, "update_type", 128);
    if (event.type != "bot_started" && event.type != "bot_stopped" && event.type != "dialog_removed"
        && event.type != "message_created" && event.type != "message_callback") return std::nullopt;
    event.timestamp = integer_field(update, "timestamp");
    if (event.timestamp < 0) invalid_update();
    if (event.type == "bot_started" || event.type == "bot_stopped" || event.type == "dialog_removed") {
        const auto& user = object_field(update, "user");
        if (!human(user)) return std::nullopt;
        event.user_id = identity(user, "user_id");
        event.display_name = user.value("first_name", std::string("Участник MAX"));
        event.chat_id = identity(update, "chat_id");
        event.activity = event.type == "bot_started";
        event.command = *event.activity ? "start" : "stop";
        // Profile fields, locale and deep-link payload cannot affect replay identity.
        event.key = "state:" + sha256(Json::array({event.type, event.timestamp, event.user_id, event.chat_id}).dump());
        return event;
    }
    if (!update.contains("message") || update.at("message").is_null()) {
        if (event.type == "message_callback") return std::nullopt;
        invalid_update();
    }
    const auto& message = object_field(update, "message");
    const auto& recipient = object_field(message, "recipient");
    if (string_field(recipient, "chat_type", 64) != "dialog") return std::nullopt;
    event.chat_id = identity(recipient, "chat_id");
    if (event.type == "message_created") {
        const auto& sender = object_field(message, "sender");
        if (!human(sender)) return std::nullopt;
        event.user_id = identity(sender, "user_id");
        const auto& body = object_field(message, "body");
        event.key = "message:" + string_field(body, "mid");
        event.command = command_from_text(body);
        // An existing chat may predate webhook setup. Explicit /start or /menu
        // establishes its generation; ordinary text/help cannot reactivate it.
        if(event.command=="start")event.activity=true;
        event.display_name = sender.value("first_name", std::string("Участник MAX"));
        if(event.command=="text") {
            event.text=body.at("text").get<std::string>();
            if(event.text.size()>16000)event.text.clear();
        }
    } else {
        const auto& callback = object_field(update, "callback");
        const auto& user = object_field(callback, "user");
        if (!human(user)) return std::nullopt;
        event.user_id = identity(user, "user_id");
        event.callback_id = string_field(callback, "callback_id");
        event.key = "callback:" + event.callback_id;
        event.display_name = user.value("first_name", std::string("Участник MAX"));
        if(callback.contains("payload")&&callback.at("payload").is_string()) {
            event.payload=callback.at("payload").get<std::string>();
            if(event.payload.size()>256)event.payload.clear();
        }
        event.command="unknown";
        for(const auto* command:{"start","help","rules","ask","knowledge","stats","requests","cancel","back","resume"})
            if(event.payload==command)event.command=command;
    }
    return event;
}

std::optional<Outbound> response_for(const BotEvent& event, const std::string& bot_username) {
    if (event.activity == false) return std::nullopt;
    return bot_reply(event,message_body(bot_username,event.command=="help"));
}
Json event_json(const BotEvent& event) {
    Json value{{"key", event.key}, {"type", event.type}, {"user_id", event.user_id}, {"chat_id", event.chat_id},
               {"command", event.command}, {"callback_id", event.callback_id}, {"timestamp", event.timestamp},
               {"text",event.text},{"payload",event.payload},{"display_name",event.display_name.size()<=400?event.display_name:"Участник MAX"}};
    value["activity"] = event.activity.has_value() ? Json(*event.activity) : Json(nullptr);
    return value;
}
BotEvent event_from_json(const Json& json) {
    BotEvent value;
    value.key = json.at("key").get<std::string>(); value.type = json.at("type").get<std::string>();
    value.user_id = json.at("user_id").get<std::string>(); value.chat_id = json.at("chat_id").get<std::string>();
    value.command = json.at("command").get<std::string>(); value.callback_id = json.at("callback_id").get<std::string>();
    value.timestamp = json.at("timestamp").get<std::int64_t>();
    value.text=json.value("text",std::string{});value.payload=json.value("payload",std::string{});
    value.display_name=json.value("display_name",std::string("Участник MAX"));
    if (json.contains("activity") && !json.at("activity").is_null()) value.activity = json.at("activity").get<bool>();
    return value;
}
Json outbound_json(const Outbound& outbound) {
    return Json{{"kind", outbound.kind}, {"chat_id", outbound.chat_id}, {"callback_id", outbound.callback_id}, {"body", outbound.body}};
}
Outbound outbound_from_json(const Json& json) {
    return {json.at("kind").get<std::string>(), json.at("chat_id").get<std::string>(),
            json.at("callback_id").get<std::string>(), json.at("body")};
}
MaxApiError::MaxApiError(int status_value, bool retryable_value, int retry_after_value, const std::string& message)
    : std::runtime_error(message), status(status_value), retryable(retryable_value), retry_after_seconds(retry_after_value) {}
void check_max_response(const HttpResult& result) {
    if (result.status < 200 || result.status >= 300)
        throw MaxApiError(result.status, result.status == 429 || (result.status >= 500 && result.status < 600),
                          result.retry_after_seconds, "MAX HTTP request failed");
    if (result.body.size() > response_limit) throw MaxApiError(result.status, false, 0, "MAX response exceeded size limit");
    Json value;
    try { value = parse_json_strict(result.body); }
    catch (...) { throw MaxApiError(result.status, false, 0, "MAX returned invalid JSON"); }
    if (!value.is_object()) throw MaxApiError(result.status, false, 0, "MAX returned an invalid response");
    if (value.contains("success") && (!value.at("success").is_boolean() || !value.at("success").get<bool>()))
        throw MaxApiError(result.status, false, 0, "MAX reported an unsuccessful operation");
    if (value.contains("error") || value.contains("code"))
        throw MaxApiError(result.status, false, 0, "MAX returned an API error");
    if (!value.contains("success") && (!value.contains("message") || !value.at("message").is_object()))
        throw MaxApiError(result.status, false, 0, "MAX returned an incomplete response");
}
MaxClient::MaxClient(std::string token, std::string base_url, MaxTransport transport, std::string ca_file)
    : token_(std::move(token)), base_url_(std::move(base_url)), ca_file_(std::move(ca_file)), transport_(std::move(transport)) {
    static const std::regex allowed_base(R"(^https://[A-Za-z0-9.-]+(:[0-9]{1,5})?(/[A-Za-z0-9_./-]*)?$)");
    if (base_url_.size() > 2048 || !std::regex_match(base_url_, allowed_base))
        throw AppError(500, "invalid_max_base_url", "MAX API base URL must use HTTPS without credentials, query or fragment");
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
    if (token_.size() > 4096 || std::any_of(token_.begin(), token_.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
        throw AppError(500, "invalid_max_token", "Invalid MAX token configuration");
    if (!transport_) transport_ = curl_send;
}
void MaxClient::send(const Outbound& outbound) const {
    if (token_.empty()) throw MaxApiError(0, false, 0, "MAX bot token is not configured");
    if (!outbound.body.is_object()) throw MaxApiError(0, false, 0, "Invalid outbound MAX body");
    std::string endpoint;
    if (outbound.kind == "message" && !outbound.chat_id.empty()) endpoint = "/messages?chat_id=" + url_encode(outbound.chat_id);
    else if (outbound.kind == "answer" && !outbound.callback_id.empty()) endpoint = "/answers?callback_id=" + url_encode(outbound.callback_id);
    else throw MaxApiError(0, false, 0, "Invalid outbound MAX destination");
    check_max_response(transport_(HttpRequest{base_url_ + endpoint, token_, outbound.body.dump(), ca_file_}));
}
} // namespace maxhelp
