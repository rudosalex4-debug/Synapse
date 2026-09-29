#pragma once
#include "config.hpp"
#include "db.hpp"
#include "max_bot.hpp"
namespace maxhelp {
bool process_inbox(Db &,const Config &);
bool process_outbox(Db &,const Config &,MaxTransport transport={});
void run_worker(const Config &);
}
