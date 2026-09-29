#include "config.hpp"
#include "domain.hpp"
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <charconv>
#include <cstdint>
#include <filesystem>
namespace maxhelp {
namespace {
std::string env(const char *name, const char *fallback="") {
 const char *v=std::getenv(name); return v?std::string(v):std::string(fallback);
}
int number(const char *name,int fallback,int min,int max) {
 auto raw=env(name); if(raw.empty())return fallback;
 int n=0; auto [end,ec]=std::from_chars(raw.data(),raw.data()+raw.size(),n);
 if(ec!=std::errc()||end!=raw.data()+raw.size()||n<min||n>max)throw std::runtime_error(std::string("Invalid setting: ")+name);
 return n;
}
}
Config read_config() {
 Config c;
 c.database_url=env("DATABASE_URL");
 if(c.database_url.empty())throw std::runtime_error("DATABASE_URL is required");
 c.app_env=env("APP_ENV","development");
 if(c.app_env!="development"&&c.app_env!="production"&&c.app_env!="test")throw std::runtime_error("Invalid APP_ENV");
 const auto demo=env("DEMO_MODE","false");
 if(demo!="true"&&demo!="false")throw std::runtime_error("Invalid DEMO_MODE");
 c.demo=demo=="true";
 if(c.app_env=="production"&&c.demo)throw std::runtime_error("DEMO_MODE is forbidden in production");
 const auto pilot=env("PILOT_MODE","false"), delivery=env("MAX_DELIVERY_ENABLED","true");
 if(pilot!="true"&&pilot!="false")throw std::runtime_error("Invalid PILOT_MODE");
 if(delivery!="true"&&delivery!="false")throw std::runtime_error("Invalid MAX_DELIVERY_ENABLED");
 c.pilot_mode=pilot=="true";c.max_delivery_enabled=delivery=="true";
 const auto product=env("MAX_PRODUCT_NOTIFICATIONS_ENABLED","false");
 if(product!="true"&&product!="false")throw std::runtime_error("Invalid MAX_PRODUCT_NOTIFICATIONS_ENABLED");
 c.product_notifications_enabled=product=="true";
 if(c.pilot_mode&&c.demo)throw std::runtime_error("Closed pilot cannot use demo login");
 auto allowed=env("PILOT_ALLOWED_MAX_IDS");
 if(allowed.size()>12000)throw std::runtime_error("Pilot allowlist too large");
 for(std::size_t offset=0;offset<allowed.size();){
  auto end=allowed.find(',',offset);auto item=allowed.substr(offset,end==std::string::npos?end:end-offset);
  auto first=item.find_first_not_of(" \t");auto last=item.find_last_not_of(" \t");
  if(first==std::string::npos)throw std::runtime_error("Empty pilot account ID");
  item=item.substr(first,last-first+1);std::int64_t id=0;
  const auto parsed=std::from_chars(item.data(),item.data()+item.size(),id);
  if(parsed.ec!=std::errc{}||parsed.ptr!=item.data()+item.size()||id<=0||std::to_string(id)!=item)
   throw std::runtime_error("Invalid pilot account ID");
  c.pilot_allowed_max_ids.insert(item);
  if(c.pilot_allowed_max_ids.size()>500)throw std::runtime_error("Pilot supports at most 500 account IDs");
  if(end==std::string::npos)break;
  offset=end+1;if(offset==allowed.size())throw std::runtime_error("Empty pilot account ID");
 }
 const auto moderators=env("MODERATOR_MAX_IDS");
 if(moderators.size()>1200)throw std::runtime_error("Moderator allowlist too large");
 for(std::size_t offset=0;offset<moderators.size();){
  const auto end=moderators.find(',',offset);auto item=moderators.substr(offset,end==std::string::npos?end:end-offset);
  const auto first=item.find_first_not_of(" \t"),last=item.find_last_not_of(" \t");
  if(first==std::string::npos)throw std::runtime_error("Empty moderator account ID");
  item=item.substr(first,last-first+1);std::int64_t id=0;
  const auto parsed=std::from_chars(item.data(),item.data()+item.size(),id);
  if(parsed.ec!=std::errc{}||parsed.ptr!=item.data()+item.size()||id<=0||std::to_string(id)!=item)
   throw std::runtime_error("Invalid moderator account ID");
  c.moderator_max_ids.insert(item);
  if(c.moderator_max_ids.size()>50)throw std::runtime_error("At most 50 moderators are supported");
  if(end==std::string::npos)break;
  offset=end+1;if(offset==moderators.size())throw std::runtime_error("Empty moderator account ID");
 }
 c.max_ca_file=env("MAX_CA_FILE");
 if(!c.max_ca_file.empty()&&(!std::filesystem::is_regular_file(c.max_ca_file)||std::filesystem::file_size(c.max_ca_file)>131072))
  throw std::runtime_error("MAX_CA_FILE must name a readable CA bundle of at most 128 KiB");
 c.token=env("MAX_BOT_TOKEN"); if(c.token.starts_with("REPLACE_"))c.token.clear();
 c.webhook_secret=env("MAX_WEBHOOK_SECRET");
 c.bot_username=env("MAX_BOT_USERNAME");
 c.api_base_url=env("MAX_API_BASE_URL","https://platform-api2.max.ru");
 c.public_url=env("APP_PUBLIC_URL","http://localhost:8080");
 if(!c.api_base_url.starts_with("https://")||c.api_base_url.find('@')!=std::string::npos)throw std::runtime_error("MAX_API_BASE_URL must be HTTPS without credentials");
 if(c.app_env=="production"&&(!c.public_url.starts_with("https://")||c.webhook_secret.size()<32))throw std::runtime_error("Production requires HTTPS APP_PUBLIC_URL and webhook secret of at least 32 characters");
 c.port=number("PORT",8080,1,65535);
 c.auth_max_age=number("AUTH_MAX_AGE_SECONDS",3600,60,3600);
 c.session_ttl=number("SESSION_TTL_SECONDS",3600,60,86400);
 c.data_dir=env("DATA_DIR","data");c.migrations_dir=env("MIGRATIONS_DIR","db/migrations");c.static_dir=env("STATIC_DIR","apps/miniapp/dist");
 return c;
}
bool pilot_allows(const Config& config,const std::string& id) {
 return !config.pilot_mode||config.pilot_allowed_max_ids.contains(id);
}
void log_event(const std::string &event,const std::string &code) {
 static std::mutex m; std::lock_guard lock(m);
 std::cerr<<Json{{"event",event},{"code",code}}.dump()<<std::endl;
}
}
