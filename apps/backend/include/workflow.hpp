#pragma once
#include "db.hpp"
#include "config.hpp"
#include "domain.hpp"
#include <map>
namespace maxhelp {
// HTTP caller authenticates the session. Bot caller verifies the MAX dialog and
// pilot access; own_transaction=false joins its inbox transaction/savepoint.
Json workflow(Db&, const Config&, const Json& catalog, const std::string& actor,
              const std::string& method, const std::string& path,
              const Json& body, const std::string& idempotency_key,
              const std::string& if_match,
              const std::map<std::string,std::string>& query,bool own_transaction=true);
}
