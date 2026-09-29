#pragma once
#include "config.hpp"
#include "db.hpp"
#include "domain.hpp"
namespace maxhelp {
Json load_catalog(const Config &);
void migrate(const Config &);
void seed(const Config &,const Json &);
Json create_login(Db &,const Config &,const Json &body,bool demo);
std::string authenticate(Db &,const Config &,const std::string &authorization);
void logout(Db &,const std::string &authorization);
Json get_profile(Db &,const std::string &id,bool own_transaction=true);
// false joins the bot inbox transaction; HTTP uses the default transaction.
Json save_profile(Db &,const Json &catalog,const std::string &id,const Json &input,const std::string &if_match,bool own_transaction=true,const Config* config=nullptr);
void accept_webhook(Db &,const Json &body);
}
