#include "jobs.hpp"
#include "domain.hpp"
#include "max_bot.hpp"
#include "maintenance.hpp"
#include "notifications.hpp"
#include "bot_workflow.hpp"
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>
#include <algorithm>
#include <random>
namespace maxhelp {
namespace {volatile std::sig_atomic_t stopping=0;void stop(int){stopping=1;}}
bool process_inbox(Db &db,const Config &config){
 Transaction tx(db);
 auto rows=db.exec("SELECT i.id,i.payload,i.event_key FROM bot_inbox i WHERE i.status='pending' AND i.available_at<=now() "
  "AND NOT EXISTS(SELECT 1 FROM bot_inbox earlier WHERE earlier.status='pending' AND earlier.id<i.id "
  "AND earlier.payload->>'user_id'=i.payload->>'user_id') ORDER BY i.id FOR UPDATE OF i SKIP LOCKED LIMIT 1");
 if(!rows.size()){tx.commit();return false;}
 const auto id=rows.get(0,"id");
 try{
  const auto event=event_from_json(Json::parse(rows.get(0,"payload")));
  auto dialog=db.exec("SELECT active,chat_id,event_timestamp FROM max_dialogs WHERE max_user_id=$1 FOR UPDATE",{event.user_id});
  if(pilot_allows(config,event.user_id)&&event.activity!=false&&dialog.size()&&dialog.get(0,"active")=="t"&&dialog.get(0,"chat_id")==event.chat_id&&std::stoll(dialog.get(0,"event_timestamp"))<=event.timestamp){
   db.exec("SAVEPOINT bot_action");
   Outbound response;
   try{response=bot_workflow(db,config,event);}
   catch(const AppError& error){
    db.exec("ROLLBACK TO SAVEPOINT bot_action");
    response=bot_reply(event,bot_message(std::string(error.what())+"\n\nВвод сохранён. /resume — продолжить, /back — исправить предыдущий шаг, /cancel — отменить.",bot_menu(config.bot_username)));
   }
   catch(const Json::exception&){db.exec("ROLLBACK TO SAVEPOINT bot_action");throw;}
   db.exec("RELEASE SAVEPOINT bot_action");
   db.exec("INSERT INTO outbox(event_key,kind,chat_id,max_user_id,payload,event_timestamp) VALUES($1,$2,$3,$4,$5::jsonb,$6::bigint) ON CONFLICT(event_key) DO NOTHING",
   {event.key,response.kind,event.chat_id,event.user_id,outbound_json(response).dump(),std::to_string(event.timestamp)});
  }
  db.exec("UPDATE bot_inbox SET status='done',attempts=attempts+1 WHERE id=$1::bigint",{id});
 }catch(const AppError&){db.exec("UPDATE bot_inbox SET status='dead',last_error='INVALID_EVENT' WHERE id=$1::bigint",{id});}
 catch(const Json::exception&){db.exec("UPDATE bot_inbox SET status='dead',last_error='INVALID_EVENT' WHERE id=$1::bigint",{id});}
 tx.commit();return true;
}
bool process_outbox(Db &db,const Config &config,MaxTransport transport){
 if(config.token.empty()||!config.max_delivery_enabled)return false;
 std::string id,lease;Json payload;int attempts=0;
 {
  Transaction tx(db);
  db.exec("UPDATE outbox SET status='dead',last_error='ATTEMPTS_EXHAUSTED',lease_token=NULL,lock_until=NULL WHERE attempts>=5 AND (status='pending' OR (status='processing' AND lock_until<now()))");
  // Forms and optional notifications keep separate FIFO sequences per chat.
  // A deferred notification must not hold up an interactive reply; within each
  // sequence retries still prevent later messages from overtaking earlier ones.
  auto rows=db.exec("SELECT o.id,o.payload,o.max_user_id,o.attempts,o.chat_id,o.event_timestamp,o.product_context FROM outbox o "
   "WHERE o.attempts<5 AND o.created_at>now()-interval '24 hours' "
   "AND ((o.status='pending' AND o.available_at<=now()) OR (o.status='processing' AND o.lock_until<now())) "
   "AND NOT EXISTS(SELECT 1 FROM outbox earlier WHERE earlier.chat_id=o.chat_id AND earlier.id<o.id "
   "AND (earlier.product_context IS NULL)=(o.product_context IS NULL) "
   "AND earlier.status IN ('pending','processing') AND earlier.created_at>now()-interval '24 hours') "
   "ORDER BY o.id FOR UPDATE OF o SKIP LOCKED LIMIT 1");
  if(!rows.size()){tx.commit();return false;}
  id=rows.get(0,"id");
  auto active=db.exec("SELECT active,chat_id,event_timestamp FROM max_dialogs WHERE max_user_id=$1",{rows.get(0,"max_user_id")});
  if(!pilot_allows(config,rows.get(0,"max_user_id"))||!active.size()||active.get(0,"active")!="t"||active.get(0,"chat_id")!=rows.get(0,"chat_id")||std::stoll(active.get(0,"event_timestamp"))>std::stoll(rows.get(0,"event_timestamp"))){db.exec("UPDATE outbox SET status='cancelled',lease_token=NULL,lock_until=NULL WHERE id=$1::bigint",{id});tx.commit();return true;}
  if(!rows.is_null(0,"product_context")){
   bool deliverable=false;
   try{const auto metadata=Json::parse(rows.get(0,"product_context"));
    deliverable=product_notification_deliverable(db,config,metadata)&&reserve_product_notification_delivery(db,metadata);
   }
   catch(const Json::exception&){deliverable=false;}
   if(!deliverable){db.exec("UPDATE outbox SET status='cancelled',last_error='PRODUCT_NOTIFICATION_STALE',lease_token=NULL,lock_until=NULL WHERE id=$1::bigint",{id});tx.commit();return true;}
  }
  payload=Json::parse(rows.get(0,"payload"));attempts=std::stoi(rows.get(0,"attempts"))+1;lease=uuid();
  db.exec("UPDATE outbox SET status='processing',attempts=attempts+1,lock_until=now()+interval '30 seconds',lease_token=$2::uuid WHERE id=$1::bigint",{id,lease});
  tx.commit();
 }
 try{
  MaxClient(config.token,config.api_base_url,std::move(transport),config.max_ca_file).send(outbound_from_json(payload));
  db.exec("UPDATE outbox SET status='done',lease_token=NULL,lock_until=NULL,last_error=NULL WHERE id=$1::bigint AND lease_token=$2::uuid",{id,lease});
  log_event("delivery_done");
 }catch(const MaxApiError&e){
  const bool retry=e.retryable&&attempts<5;
  thread_local std::mt19937 engine(std::random_device{}());
  int delay=std::min(300,(1<<attempts)*2)+std::uniform_int_distribution<int>(0,3)(engine);
  delay=std::max(delay,std::clamp(e.retry_after_seconds,0,3600));
  db.exec("UPDATE outbox SET status=$3,last_error=$4,available_at=now()+$5::int*interval '1 second',lease_token=NULL,lock_until=NULL WHERE id=$1::bigint AND lease_token=$2::uuid",
   {id,lease,retry?"pending":"dead","MAX_"+std::to_string(e.status),std::to_string(delay)});
  log_event(retry?"delivery_retry":"delivery_dead","MAX_"+std::to_string(e.status));
 }catch(const AppError&){
  db.exec("UPDATE outbox SET status='dead',last_error='INVALID_JOB',lease_token=NULL,lock_until=NULL WHERE id=$1::bigint AND lease_token=$2::uuid",{id,lease});
 }
 return true;
}
void run_worker(const Config &config){
 std::signal(SIGTERM,stop);std::signal(SIGINT,stop);
 Db db(config.database_url);int cycles=0;
 log_event("worker_started",(config.token.empty()||!config.max_delivery_enabled)?"MAX_DELIVERY_PAUSED":"MAX_DELIVERY_ENABLED");
 while(!stopping){
  try{
   const bool inbound=process_inbox(db,config);
   const bool outbound=process_outbox(db,config);
   process_matching_notifications(db,config);
   if(++cycles>=120){
    maintain_workflow(db);
    db.exec("DELETE FROM sessions WHERE expires_at<now()");
    db.exec("DELETE FROM bot_inbox WHERE status IN ('done','dead') AND received_at<now()-interval '14 days'");
    db.exec("UPDATE outbox SET status='dead',last_error='DELIVERY_EXPIRED',lease_token=NULL,lock_until=NULL WHERE status IN ('pending','processing') AND created_at<now()-interval '24 hours'");
    db.exec("DELETE FROM outbox WHERE status IN ('done','dead','cancelled') AND created_at<now()-interval '14 days'");
    cycles=0;
   }
   // One delivery worker per bot: stay below MAX's two replies/second per dialog.
   if(outbound)std::this_thread::sleep_for(std::chrono::milliseconds(550));
   else if(!inbound)std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }catch(const std::exception&){log_event("worker_retry","STORAGE_OR_JOB_FAILURE");std::this_thread::sleep_for(std::chrono::seconds(2));}
 }
 log_event("worker_stopped");
}
}


