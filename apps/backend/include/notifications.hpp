#pragma once
#include "config.hpp"
#include "db.hpp"
#include "domain.hpp"
namespace maxhelp {
Json notification_settings(Db&,const Config&,const std::string& actor);
Json save_notification_settings(Db&,const Config&,const std::string& actor,const Json& body);
// Called inside the publishing transaction, after the request becomes open.
// Records only new publications; worker delivery flags do not gate publication.
void enqueue_matching_request(Db&,const Config&,const std::string& request);
// Caller holds the user lock in a profile-save transaction; call for a changed
// competency or a transition from paused to available. Coalesces repeated saves.
void enqueue_matching_profile(Db&,const Config&,const std::string& actor);
// Processes one publication (<=1000 competencies, <=5 lifetime recipients) or
// one profile refresh (<=20 requests). Helpers: <=3 alerts/24h, spaced >=6h.
bool process_matching_notifications(Db&,const Config&);
// Caller holds the workflow transaction and both users' locks in sorted order.
void enqueue_product_notification(Db&,const Config&,const std::string& event_kind,const std::string& event_id,
 const std::string& recipient,const std::string& sender,const std::string& request,const std::string& conversation="");
bool product_notification_deliverable(Db&,const Config&,const Json& metadata);
// Caller owns the outbox claim transaction. Enforces the budget at send time.
bool reserve_product_notification_delivery(Db&,const Json& metadata);
}

