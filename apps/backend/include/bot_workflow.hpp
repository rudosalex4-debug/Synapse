#pragma once
#include "services.hpp"
#include "max_bot.hpp"
namespace maxhelp {
// Caller owns inbox transaction and holds the active, allowed MAX dialog lock.
Outbound bot_workflow(Db&,const Config&,const BotEvent&);
}
