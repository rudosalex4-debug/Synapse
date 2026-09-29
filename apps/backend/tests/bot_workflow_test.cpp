#include "jobs.hpp"
#include "services.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace maxhelp;
namespace {
int checks=0;
bool cleanup_failed=false;
void check(bool value,const std::string& name){if(!value)throw std::runtime_error(name);++checks;}
struct Fixture {
 Db& db;Config config;std::string user,actor,tag;long long clock=100;int sequence=0;
 Fixture(Db& connection,const Config& settings):db(connection),config(settings),tag("bot-test:"+uuid()){
  user=std::to_string(std::stoll(sha256(tag).substr(0,15),nullptr,16)+1);
  auto event=Json{{"update_type","bot_started"},{"timestamp",clock},{"chat_id",-std::stoll(user)},
   {"user",{{"user_id",std::stoll(user)},{"is_bot",false},{"first_name","Проверка"}}}};
  accept_webhook(db,event);check(process_inbox(db,config),"start processed");
 }
 ~Fixture(){try{
  Transaction tx(db);
  db.exec("DELETE FROM help_offers WHERE request_id IN (SELECT id FROM help_requests WHERE author_id IN (SELECT id FROM users WHERE max_user_id=$1))",{user});
  db.exec("DELETE FROM workflow_idempotency WHERE actor_id IN (SELECT id FROM users WHERE max_user_id=$1)",{user});
  db.exec("DELETE FROM help_requests WHERE author_id IN (SELECT id FROM users WHERE max_user_id=$1)",{user});
  db.exec("DELETE FROM users WHERE max_user_id=$1 OR max_user_id LIKE $2",{user,tag+":%"});
  db.exec("DELETE FROM bot_inbox WHERE payload->>'user_id'=$1",{user});
  db.exec("DELETE FROM outbox WHERE max_user_id=$1",{user});
  db.exec("DELETE FROM max_dialogs WHERE max_user_id=$1",{user});tx.commit();
 }catch(...){cleanup_failed=true;std::cerr<<"Bot test cleanup failed\n";}}
 Json update(const std::string& text,bool callback=false){
  Json message={{"recipient",{{"chat_type","dialog"},{"chat_id",-std::stoll(user)}}},
   {"sender",{{"user_id",std::stoll(user)},{"is_bot",false},{"first_name","Проверка"}}},
   {"body",{{"mid",tag+":"+std::to_string(++sequence)},{"text",text}}}};
  Json event={{"update_type",callback?"message_callback":"message_created"},{"timestamp",++clock},{"message",message}};
  if(callback)event["callback"]={{"callback_id",tag+":cb:"+std::to_string(sequence)},{"payload",text},
   {"user",{{"user_id",std::stoll(user)},{"is_bot",false},{"first_name","Проверка"}}}};
  return event;
 }
 Json send(const Json& event){
  auto parsed=*parse_update(event);accept_webhook(db,event);
  check(process_inbox(db,config),"event processed");
  auto job=db.exec("SELECT payload FROM outbox WHERE event_key=$1",{parsed.key});
  check(job.size()==1,"one reply committed");auto reply=outbound_from_json(Json::parse(job.get(0,"payload")));
  return reply.kind=="answer"?reply.body.at("message"):reply.body;
 }
 Json text(const std::string& value){return send(update(value));}
 Json callback(const std::string& value){return send(update(value,true));}
 Json state(){auto r=db.exec("SELECT state FROM bot_forms WHERE max_user_id=$1",{user});return r.size()?Json::parse(r.get(0,"state")):Json();}
 std::string action(const std::string& value){return "form:"+state().at("nonce").get<std::string>()+":"+value;}
 Json tap(const std::string& value){return callback(action(value));}
 void topic(){tap("topic:science");tap("topic:science.math");tap("topic_done");}
 void knowledge(){text("/knowledge");topic();text("Объясняю дроби и действия с ними");tap("experience:teaching");tap("available:yes");}
 std::string request(){
  text("/ask");text("Как складывать дроби?");text("Не понимаю, как привести дроби к общему знаменателю. Пробовал складывать числители.");topic();tap("goal:understand");tap("save");
  return db.exec("SELECT id FROM help_requests WHERE author_id=(SELECT id FROM users WHERE max_user_id=$1) ORDER BY created_at DESC LIMIT 1",{user}).get(0,"id");
 }
};
void run(Db& db,const Config& config){
 Fixture a(db,config),b(db,config);
 db.exec("DELETE FROM max_dialogs WHERE max_user_id=$1",{b.user});
 b.text("/start");
 check(db.exec("SELECT active FROM max_dialogs WHERE max_user_id=$1",{b.user}).get(0,"active")=="t","start activates chat opened before webhook setup");
 a.text("/knowledge");const auto old=a.action("topic:science");a.tap("topic:science");
 a.callback(old);check(a.state()["data"]["topicId"]=="science","stale button cannot advance form");
 a.text("/cancel");check(a.state().is_null(),"cancel removes form");
 a.knowledge();
 auto actor=db.exec("SELECT id FROM users WHERE max_user_id=$1",{a.user}).get(0,"id");
 db.exec("UPDATE users SET bio='Изменено в mini-app',revision=revision+1 WHERE id=$1::uuid",{actor});
 a.tap("save");check(!a.state().is_null(),"profile conflict requests confirmation");
 const auto save=a.update(a.action("save"),true);a.send(save);
 check(a.state().is_null(),"successful save clears form");
 accept_webhook(db,save);check(!process_inbox(db,config),"duplicate save not reprocessed");
 auto profile=get_profile(db,actor);
 check(profile["competencies"].size()==1&&profile["bio"]=="Изменено в mini-app","knowledge shares profile and retains concurrent fields");
 check(profile["competencies"][0]["experienceKind"]=="teaching","experience preserved");
 a.knowledge();a.tap("save");check(get_profile(db,actor)["competencies"].size()==2,"adding knowledge retains existing competencies");
 a.text("/ask");a.text("Заголовок");auto invalid=a.text("коротко");
 check(a.state()["step"]=="body"&&invalid["text"].get<std::string>().find("30")!=std::string::npos,"validation keeps current form");
 {Db restarted(config.database_url);auto resume=a.update("/resume");accept_webhook(restarted,resume);check(process_inbox(restarted,config),"another worker resumes persisted form");}
 a.text("/cancel");auto request=a.request();
 check(db.exec("SELECT status FROM help_requests WHERE id=$1::uuid",{request}).get(0,"status")=="draft","bot saves private draft first");
 auto denied=b.callback("publish:"+request+":0");
 check(denied["text"].get<std::string>().find("недоступен")!=std::string::npos,"other user cannot publish draft");
 a.callback("publish:"+request+":0");
 check(db.exec("SELECT status FROM help_requests WHERE id=$1::uuid",{request}).get(0,"status")=="open","explicit publish uses workflow");
 a.callback("publish:"+request+":0");
 check(db.exec("SELECT revision FROM help_requests WHERE id=$1::uuid",{request}).get(0,"revision")=="1","repeated publish cannot mutate twice");
 const auto changed=a.request();
 db.exec("UPDATE help_requests SET title='Правка из mini-app',revision=revision+1 WHERE id=$1::uuid",{changed});
 a.callback("publish:"+changed+":0");
 check(db.exec("SELECT status FROM help_requests WHERE id=$1::uuid",{changed}).get(0,"status")=="draft","stale preview cannot publish changed draft");
 a.callback("request:"+changed);a.callback("publish:"+changed+":1");
 check(db.exec("SELECT status FROM help_requests WHERE id=$1::uuid",{changed}).get(0,"status")=="open","fresh preview can publish changed draft");
 // More than the HTTP offers page limit: totals must cover all respondents.
 db.exec("INSERT INTO users(id,max_user_id,display_name,provenance) SELECT gen_random_uuid(),$1||':'||n,'Helper','self_declared' FROM generate_series(1,101) n",{a.tag});
 db.exec("INSERT INTO help_offers(id,request_id,helper_id,message,status,competency_snapshot,score,narrower) "
  "SELECT gen_random_uuid(),$1::uuid,id,'Могу помочь','pending','{}'::jsonb,50,false FROM users WHERE max_user_id LIKE $2",{request,a.tag+":%"});
 auto details=a.callback("request:"+request);
 check(details["text"].get<std::string>().find("Откликов: 101")!=std::string::npos,"request totals exceed page limit");
 auto stats=a.text("/stats");check(stats["text"].get<std::string>().find("Уникальных откликнувшихся: 101")!=std::string::npos,"unique respondents counted");
 auto private_stats=b.text("/stats");check(private_stats["text"].get<std::string>().find("101")==std::string::npos,"statistics scoped to sender");
 // Same account's later input cannot jump over a claimed earlier event.
 auto first=a.update("/ask"),second=a.update("Новый заголовок");accept_webhook(db,first);accept_webhook(db,second);
 {Db claimant(config.database_url);Transaction lock(claimant);
  claimant.exec("SELECT id FROM bot_inbox WHERE event_key=$1 FOR UPDATE",{parse_update(first)->key});
  check(!process_inbox(db,config),"later same-user event waits for earlier claim");
 }
 check(process_inbox(db,config)&&process_inbox(db,config),"ordered events drain");
 check(a.state()["step"]=="body","input consumed at correct step");
 Config closed=config;closed.pilot_mode=true;
 auto blocked=a.update("/cancel");accept_webhook(db,blocked);check(process_inbox(db,closed),"revoked pilot event consumed");
 check(!a.state().is_null(),"revoked access cannot mutate form");
 check(db.exec("SELECT id FROM outbox WHERE event_key=$1",{parse_update(blocked)->key}).size()==0,"revoked pilot cannot receive personal response");
 a.text("/cancel");
 a.text("/knowledge");
 db.exec("UPDATE bot_forms SET state=jsonb_set(state,'{data,topicId}','\"removed.topic\"'::jsonb) WHERE max_user_id=$1",{a.user});
 a.tap("topic:science");check(a.state()["data"]["topicId"]=="science","removed catalog topic can be reselected");
 auto stop=Json{{"update_type","bot_stopped"},{"timestamp",++a.clock},{"chat_id",-std::stoll(a.user)},
  {"user",{{"user_id",std::stoll(a.user)},{"is_bot",false}}}};
 accept_webhook(db,stop);check(a.state().is_null(),"stop removes unfinished form");
 check(process_inbox(db,config),"stop processed without reply");
}
}
int main(){try{
 const auto* url=std::getenv("DATABASE_URL");if(!url)throw std::runtime_error("Use a migrated disposable DATABASE_URL, with no worker");
 Config config;config.database_url=url;config.data_dir=std::getenv("DATA_DIR")?std::getenv("DATA_DIR"):SYNAPSE_DATA_DIR;config.bot_username="synthetic_bot";
 config.app_env="test";config.max_delivery_enabled=false;
 Db db(config.database_url);
 check(db.exec("SELECT id FROM bot_inbox WHERE status='pending' LIMIT 1").size()==0,"database must not have pending events");
 run(db,config);check(!cleanup_failed,"synthetic rows cleaned up");
 std::cout<<"Passed "<<checks<<" bot workflow checks; no MAX calls\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
