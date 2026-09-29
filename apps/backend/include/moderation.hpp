#pragma once
#include "config.hpp"
#include "db.hpp"
#include "domain.hpp"
#include <map>
namespace maxhelp {
bool can_moderate(Db&, const Config&, const std::string& actor);
Json moderation(Db&, const Config&, const Json& catalog, const std::string& actor,
                const std::string& method, const std::string& path, const Json& body,
                const std::string& idempotency_key, const std::map<std::string,std::string>& query);
}
