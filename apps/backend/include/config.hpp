#pragma once
#include <string>
#include <set>
namespace maxhelp {
struct Config {
 std::string database_url, app_env, token, webhook_secret, bot_username, api_base_url, public_url;
 std::string data_dir, migrations_dir, static_dir, max_ca_file;
 std::set<std::string> pilot_allowed_max_ids, moderator_max_ids;
 int port=8080, auth_max_age=3600, session_ttl=3600;
 bool demo=false, pilot_mode=false, max_delivery_enabled=true, product_notifications_enabled=false;
};
Config read_config();
bool pilot_allows(const Config&, const std::string& max_user_id);
void log_event(const std::string &event, const std::string &code="");
}
