#pragma once
#include "db.hpp"
#include "domain.hpp"
namespace maxhelp {
inline constexpr const char* community_rules_version="2026-09-27";
Json community_rules();
Json community_rules_status(Db&,const std::string& actor);
// One atomic update; also safe inside the bot inbox transaction.
Json accept_community_rules(Db&,const std::string& actor,const Json& body);
void require_current_rules(Db&,const std::string& actor);
}
