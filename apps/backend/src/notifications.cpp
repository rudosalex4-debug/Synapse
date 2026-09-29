#include "notifications.hpp"
#include "max_bot.hpp"
#include "matching.hpp"
#include "services.hpp"
#include "community_rules.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
namespace maxhelp {
namespace {
bool configured(const Config& config){
 return config.product_notifications_enabled&&!config.token.empty()&&!config.bot_username.empty();
}
bool valid_uuid(const std::string& value){
 if(value.size()!=36)return false;
 for(std::size_t i=0;i<value.size();++i){
  if(i==8||i==13||i==18||i==23){if(value[i]!='-')return false;}
  else if(!std::isxdigit(static_cast<unsigned char>(value[i])))return false;
 }return true;
}
bool valid_context(const Json& value){
 if(!value.is_object()||value.size()!=6)return false;
 for(const auto* key:{"kind","eventId","recipientId","senderId","requestId","conversationId"})
  if(!value.contains(key)||!value.at(key).is_string())return false;
 for(const auto* key:{"eventId","recipientId","senderId","requestId"})
  if(!valid_uuid(value.at(key).get<std::string>()))return false;
 const auto kind=value.at("kind").get<std::string>();
 if(kind!="offer_received"&&kind!="helper_selected"&&kind!="message_received"&&kind!="matching_request")return false;
 const auto conversation=value.at("conversationId").get<std::string>();
 if(kind=="matching_request"&&value.at("eventId")!=value.at("requestId"))return false;
 return ((kind=="offer_received"||kind=="matching_request")?conversation.empty():valid_uuid(conversation))&&value.at("recipientId")!=value.at("senderId");
}
MatchFacets facets(const Json& values){
 MatchFacets result;for(auto it=values.begin();it!=values.end();++it)
  for(const auto& value:it.value())result[it.key()].insert(value.get<std::string>());
 return result;
}
MatchQuery query_from(const Result& rows,int i){
 MatchQuery query;query.author_id=rows.get(i,"author_id");query.topic_id=rows.get(i,"topic_id");
 query.facets=facets(Json::parse(rows.get(i,"facets")));
 for(const auto& value:Json::parse(rows.get(i,"required_facets")))query.required_facets.insert(value.get<std::string>());
 if(!rows.is_null(i,"desired_experience"))query.desired_experience=rows.get(i,"desired_experience");
 query.limit=5;return query;
}
bool selectable(const Json& catalog,const std::string& id){
 for(const auto& topic:catalog.at("topics"))
  if(topic.at("id")==id)return topic.at("active").get<bool>()&&topic.at("level").get<int>()>=2;
 return false;
}
Json descendant_topics(const Json& catalog,const std::string& request_topic){
 std::map<std::string,std::string> parents;
 for(const auto& topic:catalog.at("topics"))if(topic.at("active").get<bool>())
  parents[topic.at("id").get<std::string>()]=topic.at("parent_id").is_null()?"":topic.at("parent_id").get<std::string>();
 Json result=Json::array();
 for(const auto& topic:catalog.at("topics"))if(topic.at("active").get<bool>()&&topic.at("level").get<int>()>=2){
  const auto id=topic.at("id").get<std::string>();
  for(auto current=id;!current.empty()&&parents.contains(current);current=parents.at(current))
   if(current==request_topic){result.push_back(id);break;}
 }
 return result;
}
std::vector<MatchCompetency> skills_from(const Result& rows){
 std::vector<MatchCompetency> result;result.reserve(static_cast<std::size_t>(rows.size()));
 for(int i=0;i<rows.size();++i){
  MatchCompetency skill;skill.id=rows.get(i,"id");skill.user_id=rows.get(i,"user_id");skill.topic_id=rows.get(i,"topic_id");
  skill.facets=facets(Json::parse(rows.get(i,"facets")));skill.experience=rows.get(i,"experience_kind");
  skill.available=rows.get(i,"available_to_help")=="t";
  skill.load=static_cast<unsigned>(std::stoul(rows.get(i,"load")));
  skill.capacity=static_cast<unsigned>(std::stoul(rows.get(i,"max_active_conversations")));
  skill.last_notified=std::stoll(rows.get(i,"last_notified"));result.push_back(std::move(skill));
 }
 return result;
}
const std::string skill_columns="c.id,c.user_id,c.topic_id,c.facets,c.experience_kind,u.available_to_help,u.max_active_conversations,"
 "(SELECT count(*) FROM conversations v WHERE v.helper_id=u.id AND v.status='active') AS load,"
 "COALESCE((SELECT extract(epoch FROM n.created_at)::bigint FROM matching_notification_dispatches n "
 "WHERE n.recipient_id=u.id ORDER BY n.created_at DESC LIMIT 1),0) AS last_notified ";
std::vector<MatchCompetency> helper_skills(Db& db,const std::string& helper){
 auto rows=db.exec("SELECT "+skill_columns+"FROM competencies c JOIN users u ON u.id=c.user_id "
  "JOIN topics t ON t.id=c.topic_id AND t.active WHERE u.id=$1::uuid ORDER BY c.id LIMIT 30",{helper});
 return skills_from(rows);
}
// Reads only matching inputs, never a question title/body or private evidence.
bool matching_deliverable(Db& db,const Config& config,const Json& metadata){
 if(!configured(config))return false;
 const auto recipient=metadata.at("recipientId").get<std::string>(),request=metadata.at("requestId").get<std::string>();
 auto rows=db.exec("SELECT r.author_id,r.topic_id,r.facets,r.required_facets,r.desired_experience "
  "FROM help_requests r JOIN matching_notification_jobs j ON j.request_id=r.id "
  "JOIN topics t ON t.id=r.topic_id AND t.active AND t.level>=2 JOIN users u ON u.id=$2::uuid "
  "WHERE r.id=$1::uuid AND r.author_id=$3::uuid AND r.status='open' AND r.expires_at>now() "
  "AND u.product_notifications_since<=j.created_at AND u.available_to_help "
  "AND NOT EXISTS(SELECT 1 FROM help_offers o WHERE o.request_id=r.id AND o.helper_id=u.id) "
  "AND (SELECT count(*) FROM help_offers o WHERE o.request_id=r.id)<100",
  {request,recipient,metadata.at("senderId").get<std::string>()});
 if(!rows.size())return false;
 const auto catalog=load_catalog(config);const auto query=query_from(rows,0);
 if(!selectable(catalog,query.topic_id))return false;
 MatchingIndex index(catalog,helper_skills(db,recipient));return !index.search(query).results.empty();
}
bool helper_budget(Db& db,const std::string& recipient){
 // At most three matching alerts per rolling 24h, with six hours between alerts.
 auto rows=db.exec("SELECT count(*) AS count,COALESCE(bool_or(created_at>now()-interval '6 hours'),false) AS recent "
  "FROM matching_notification_dispatches WHERE recipient_id=$1::uuid AND created_at>now()-interval '24 hours'",{recipient});
 return std::stoi(rows.get(0,"count"))<3&&rows.get(0,"recent")!="t";
}
void lock_people(Db& db,const std::set<std::string>& people){
 db.exec("SELECT id FROM users WHERE id IN (SELECT value::uuid FROM jsonb_array_elements_text($1::jsonb)) "
  "ORDER BY id FOR UPDATE",{Json(people).dump()});
}
// A profile job is coalesced by user and generation. It is read before locking
// people, never locked before its user: profile saves take the user lock first.
// Completion only touches the generation read, so a newer save is not lost.
bool process_profile_refresh(Db& db,const Config& config){
 Transaction tx(db);
 auto pending=db.exec("SELECT user_id,generation,created_at>now()-interval '24 hours' AS fresh "
  "FROM matching_notification_refreshes WHERE completed_at IS NULL ORDER BY created_at,user_id LIMIT 1");
 if(!pending.size()){tx.commit();return false;}
 const auto helper=pending.get(0,"user_id"),generation=pending.get(0,"generation");
 auto finish=[&](){db.exec("UPDATE matching_notification_refreshes SET completed_at=clock_timestamp() "
  "WHERE user_id=$1::uuid AND generation=$2::uuid",{helper,generation});tx.commit();};
 if(!configured(config)||pending.get(0,"fresh")!="t"){finish();return true;}
 const auto catalog=load_catalog(config);auto skills=helper_skills(db,helper);
 std::map<std::string,std::string> parents;std::set<std::string> topics;
 for(const auto& topic:catalog.at("topics"))if(topic.at("active").get<bool>())
  parents[topic.at("id").get<std::string>()]=topic.at("parent_id").is_null()?"":topic.at("parent_id").get<std::string>();
 for(const auto& skill:skills)for(auto current=skill.topic_id;!current.empty()&&parents.contains(current);current=parents.at(current))
  if(selectable(catalog,current))topics.insert(current);
 // LIMIT is inside the indexed topic sample, before opt-in/offer/block checks.
 auto requests=db.exec("SELECT r.id,r.author_id,r.topic_id,r.facets,r.required_facets,r.desired_experience "
  "FROM help_requests r WHERE r.status='open' AND r.topic_id IN (SELECT value FROM jsonb_array_elements_text($1::jsonb)) "
  "ORDER BY r.topic_id,r.created_at DESC,r.id DESC LIMIT 20",{Json(topics).dump()});
 std::set<std::string> people{helper};for(int i=0;i<requests.size();++i)people.insert(requests.get(i,"author_id"));
 lock_people(db,people);
 // Rebuild after acquiring profile/capacity locks; candidate query is only a hint.
 MatchingIndex index(catalog,helper_skills(db,helper));
 for(int i=0;i<requests.size();++i){
  if(!helper_budget(db,helper))break;
  if(index.search(query_from(requests,i)).results.empty())continue;
  enqueue_product_notification(db,config,"matching_request",requests.get(i,"id"),helper,requests.get(i,"author_id"),requests.get(i,"id"));
 }
 finish();return true;
}
}
Json notification_settings(Db& db,const Config& config,const std::string& actor){
 auto rows=db.exec("SELECT u.product_notifications,COALESCE(d.active,false) AS bot_started "
  "FROM users u LEFT JOIN max_dialogs d ON d.max_user_id=u.max_user_id WHERE u.id=$1::uuid",{actor});
 if(!rows.size())throw AppError(404,"USER_NOT_FOUND","Профиль не найден.");
 // Backend and worker can intentionally use different delivery flags. This is
 // configuration availability, not a worker-health or delivery-success signal.
 return {{"enabled",rows.get(0,"product_notifications")=="t"},{"botStarted",rows.get(0,"bot_started")=="t"},
  {"deliveryAvailable",configured(config)}};
}
Json save_notification_settings(Db& db,const Config& config,const std::string& actor,const Json& body){
 if(!body.is_object()||body.size()!=1||!body.contains("enabled")||!body.at("enabled").is_boolean())
  throw AppError(400,"INVALID_NOTIFICATION_SETTINGS","Укажите enabled: true или false.");
 const bool enabled=body.at("enabled").get<bool>();Transaction tx(db);
 auto users=db.exec("SELECT id FROM users WHERE id=$1::uuid FOR UPDATE",{actor});
 if(!users.size())throw AppError(404,"USER_NOT_FOUND","Профиль не найден.");
 db.exec("UPDATE users SET product_notifications=$2::boolean,product_notifications_since=CASE "
  "WHEN NOT $2::boolean THEN NULL WHEN NOT product_notifications OR product_notifications_since IS NULL "
  "THEN clock_timestamp() ELSE product_notifications_since END WHERE id=$1::uuid",{actor,enabled?"true":"false"});
 // An already claimed or sent message cannot be recalled by disabling the setting.
 if(!enabled)db.exec("UPDATE outbox SET status='cancelled',last_error='NOTIFICATIONS_DISABLED' "
  "WHERE status='pending' AND product_context->>'recipientId'=$1",{actor});
 auto result=notification_settings(db,config,actor);tx.commit();return result;
}
void enqueue_matching_request(Db& db,const Config& config,const std::string& request){
 // No backfill from existing open questions, including when the flag is enabled.
 if(!configured(config))return;
 db.exec("INSERT INTO matching_notification_jobs(request_id) "
  "SELECT id FROM help_requests WHERE id=$1::uuid AND status='open' AND expires_at>now() "
  "ON CONFLICT(request_id) DO NOTHING",{request});
}
void enqueue_matching_profile(Db& db,const Config& config,const std::string& actor){
 if(!configured(config))return;
 db.exec("INSERT INTO matching_notification_refreshes(user_id,generation) "
  "SELECT id,$2::uuid FROM users WHERE id=$1::uuid AND available_to_help AND product_notifications "
  "AND accepted_rules_version=$3 AND rules_accepted_at IS NOT NULL "
  "ON CONFLICT(user_id) DO UPDATE SET generation=excluded.generation,created_at=excluded.created_at,completed_at=NULL",
  {actor,uuid(),community_rules_version});
}
bool process_matching_notifications(Db& db,const Config& config){
 {
  Transaction tx(db);
  auto jobs=db.exec("SELECT request_id,created_at>now()-interval '24 hours' AS fresh FROM matching_notification_jobs "
   "WHERE status='pending' ORDER BY created_at,request_id FOR NO KEY UPDATE SKIP LOCKED LIMIT 1");
  if(jobs.size()){
   const auto request=jobs.get(0,"request_id");
   auto finish=[&](const std::string& status){db.exec("UPDATE matching_notification_jobs SET status=$2,completed_at=clock_timestamp() "
    "WHERE request_id=$1::uuid",{request,status});tx.commit();};
   if(!configured(config)||jobs.get(0,"fresh")!="t"){finish("skipped");return true;}
   auto rows=db.exec("SELECT r.author_id,r.topic_id,r.facets,r.required_facets,r.desired_experience FROM help_requests r "
    "JOIN topics t ON t.id=r.topic_id AND t.active WHERE r.id=$1::uuid AND r.status='open' AND r.expires_at>now()",{request});
   if(!rows.size()){finish("skipped");return true;}
   const auto catalog=load_catalog(config);auto query=query_from(rows,0);
   if(!selectable(catalog,query.topic_id)){finish("skipped");return true;}
   // Bounded BEFORE joins: this is a sample, not an exhaustive top-five search.
   auto candidates=db.exec("WITH sample AS MATERIALIZED (SELECT c.* FROM competencies c "
    "WHERE c.topic_id IN (SELECT value FROM jsonb_array_elements_text($1::jsonb)) "
    "ORDER BY c.topic_id,c.user_id,c.id LIMIT 1000) SELECT "+skill_columns+
    "FROM sample c JOIN users u ON u.id=c.user_id JOIN topics t ON t.id=c.topic_id AND t.active "
    "JOIN max_dialogs d ON d.max_user_id=u.max_user_id AND d.active "
    "JOIN matching_notification_jobs j ON j.request_id=$3::uuid "
    "WHERE c.user_id<>$2::uuid AND u.product_notifications AND u.product_notifications_since<=j.created_at "
    "AND u.available_to_help AND u.accepted_rules_version=$6 AND u.rules_accepted_at IS NOT NULL "
    "AND (NOT $4::boolean OR u.max_user_id IN (SELECT value FROM jsonb_array_elements_text($5::jsonb))) "
    "AND NOT EXISTS(SELECT 1 FROM user_blocks b WHERE (b.blocker_id=$2::uuid AND b.blocked_id=u.id) "
    "OR (b.blocked_id=$2::uuid AND b.blocker_id=u.id)) "
    "AND NOT EXISTS(SELECT 1 FROM help_offers o WHERE o.request_id=$3::uuid AND o.helper_id=u.id) "
    "AND NOT EXISTS(SELECT 1 FROM matching_notification_dispatches n WHERE n.recipient_id=u.id AND n.created_at>now()-interval '6 hours') "
    "AND (SELECT count(*) FROM matching_notification_dispatches n WHERE n.recipient_id=u.id AND n.created_at>now()-interval '24 hours')<3",
    {descendant_topics(catalog,query.topic_id).dump(),query.author_id,request,config.pilot_mode?"true":"false",
     Json(config.pilot_allowed_max_ids).dump(),community_rules_version});
   MatchingIndex index(catalog,skills_from(candidates));const auto matches=index.search(query).results;
   std::set<std::string> people{query.author_id};for(const auto& match:matches)people.insert(match.user_id);
   lock_people(db,people);
   for(const auto& match:matches)enqueue_product_notification(db,config,"matching_request",request,match.user_id,query.author_id,request);
   finish("done");return true;
  }
  tx.commit();
 }
 return process_profile_refresh(db,config);
}
bool product_notification_deliverable(Db& db,const Config& config,const Json& metadata){
 if(!config.product_notifications_enabled||!valid_context(metadata))return false;
 const auto kind=metadata.at("kind").get<std::string>(),recipient=metadata.at("recipientId").get<std::string>(),
  sender=metadata.at("senderId").get<std::string>(),request=metadata.at("requestId").get<std::string>(),
  conversation=metadata.at("conversationId").get<std::string>(),event=metadata.at("eventId").get<std::string>();
 auto people=db.exec("SELECT r.max_user_id AS recipient_max,s.max_user_id AS sender_max "
  "FROM users r JOIN users s ON s.id=$2::uuid JOIN max_dialogs d ON d.max_user_id=r.max_user_id "
  "WHERE r.id=$1::uuid AND r.product_notifications AND d.active AND s.max_user_id IS NOT NULL "
  "AND r.accepted_rules_version=$3 AND r.rules_accepted_at IS NOT NULL "
  "AND s.accepted_rules_version=$3 AND s.rules_accepted_at IS NOT NULL "
  "AND NOT EXISTS(SELECT 1 FROM user_blocks b WHERE (b.blocker_id=r.id AND b.blocked_id=s.id) "
  "OR (b.blocker_id=s.id AND b.blocked_id=r.id))",{recipient,sender,community_rules_version});
 if(!people.size()||!pilot_allows(config,people.get(0,"recipient_max"))||!pilot_allows(config,people.get(0,"sender_max")))return false;
 if(kind=="matching_request")return matching_deliverable(db,config,metadata);
 if(kind=="offer_received")return db.exec("SELECT o.id FROM help_offers o JOIN help_requests r ON r.id=o.request_id "
  "WHERE o.id=$1::uuid AND o.request_id=$2::uuid AND o.helper_id=$3::uuid AND r.author_id=$4::uuid "
  "AND o.status='pending' AND r.status='open' AND r.expires_at>now() AND EXISTS(SELECT 1 FROM topics t WHERE t.id=r.topic_id AND t.active)",{event,request,sender,recipient}).size()>0;
 const std::string active="SELECT c.id FROM conversations c JOIN help_requests r ON r.id=c.request_id "
  "WHERE c.id=$1::uuid AND c.request_id=$2::uuid AND c.status='active' AND r.status='in_progress' ";
 if(kind=="helper_selected")return db.exec(active+
  "AND c.author_id=$3::uuid AND c.helper_id=$4::uuid AND (c.id=$5::uuid OR c.offer_id=$5::uuid)",
  {conversation,request,sender,recipient,event}).size()>0;
 return db.exec(active+"AND ((c.author_id=$3::uuid AND c.helper_id=$4::uuid) OR (c.helper_id=$3::uuid AND c.author_id=$4::uuid)) "
  "AND EXISTS(SELECT 1 FROM conversation_messages m WHERE m.id=$5::uuid AND m.conversation_id=c.id AND m.sender_id=$3::uuid)",
  {conversation,request,sender,recipient,event}).size()>0;
}
bool reserve_product_notification_delivery(Db& db,const Json& metadata){
 if(!valid_context(metadata))return false;
 if(metadata.at("kind")!="matching_request")return true;
 const auto recipient=metadata.at("recipientId").get<std::string>(),request=metadata.at("requestId").get<std::string>();
 // Outbox rows are already locked here. A separate advisory lock serializes
 // delivery reservations without reversing workflow user/outbox lock order.
 db.exec("SELECT pg_advisory_xact_lock(724627,hashtext($1))",{recipient});
 auto reserved=db.exec("UPDATE matching_notification_dispatches SET delivery_reserved_at=clock_timestamp() "
  "WHERE request_id=$1::uuid AND recipient_id=$2::uuid "
  "AND NOT EXISTS(SELECT 1 FROM matching_notification_dispatches n WHERE n.recipient_id=$2::uuid "
  "AND n.request_id<>$1::uuid AND n.delivery_reserved_at>now()-interval '6 hours') "
  "AND (SELECT count(*) FROM matching_notification_dispatches n WHERE n.recipient_id=$2::uuid "
  "AND n.request_id<>$1::uuid AND n.delivery_reserved_at>now()-interval '24 hours')<3 RETURNING request_id",{request,recipient});
 // Retries renew their own reservation. Failed attempts conservatively consume
 // a slot; a paused worker cannot dump several days of queued alerts at once.
 return reserved.size()>0;
}
void enqueue_product_notification(Db& db,const Config& config,const std::string& event_kind,const std::string& event_id,
 const std::string& recipient,const std::string& sender,const std::string& request,const std::string& conversation){
 if(!config.product_notifications_enabled||config.bot_username.empty())return;
 Json metadata={{"kind",event_kind},{"eventId",event_id},{"recipientId",recipient},{"senderId",sender},
  {"requestId",request},{"conversationId",conversation}};
 if(!product_notification_deliverable(db,config,metadata))return;
 if(event_kind=="message_received"&&db.exec("SELECT id FROM outbox WHERE product_context->>'kind'='message_received' "
   "AND product_context->>'recipientId'=$1 AND product_context->>'conversationId'=$2 "
   "AND created_at>now()-interval '60 seconds' LIMIT 1",{recipient,conversation}).size())return;
 auto dialog=db.exec("SELECT u.max_user_id,d.chat_id,d.event_timestamp FROM users u JOIN max_dialogs d ON d.max_user_id=u.max_user_id "
  "WHERE u.id=$1::uuid AND d.active",{recipient});
 if(!dialog.size())return;
 if(event_kind=="matching_request"){
  // Both people are locked: the author serializes the request's lifetime cap,
  // the helper serializes the rolling daily cap across different requests.
  if(!helper_budget(db,recipient))return;
  auto inserted=db.exec("INSERT INTO matching_notification_dispatches(request_id,recipient_id) "
   "SELECT $1::uuid,$2::uuid WHERE (SELECT count(*) FROM matching_notification_dispatches WHERE request_id=$1::uuid)<5 "
   "ON CONFLICT(request_id,recipient_id) DO NOTHING RETURNING recipient_id",{request,recipient});
  if(!inserted.size())return;
 }
 const std::string text=event_kind=="offer_received"?"На ваш учебный вопрос откликнулись. Откройте Синапс, чтобы посмотреть отклик.":
  event_kind=="helper_selected"?"Вашу помощь выбрали. Откройте Синапс, чтобы перейти к переписке.":
  event_kind=="matching_request"?"Появился подходящий учебный вопрос. Откройте его в Синапсе или перейдите в раздел «Могу помочь». Вы сами решаете, откликаться ли.":
  "В вашем диалоге есть новое сообщение. Откройте Синапс, чтобы прочитать его.";
 Json button={{"type","open_app"},{"text",event_kind=="matching_request"?"Посмотреть вопрос":"Открыть Синапс"},
  {"web_app",config.bot_username},{"payload",event_kind=="matching_request"?"request_"+request:"profile"}};
 Json body={{"text",text},{"notify",true},{"attachments",Json::array({Json{{"type","inline_keyboard"},
  {"payload",Json{{"buttons",Json::array({Json::array({button})})}}}}})}};
 Outbound message{"message",dialog.get(0,"chat_id"),"",body};
 db.exec("INSERT INTO outbox(event_key,kind,chat_id,max_user_id,payload,event_timestamp,product_context) "
  "VALUES($1,'message',$2,$3,$4::jsonb,$5::bigint,$6::jsonb) ON CONFLICT(event_key) DO NOTHING",
  {"product:"+event_kind+":"+event_id+":"+recipient,dialog.get(0,"chat_id"),dialog.get(0,"max_user_id"),
   outbound_json(message).dump(),dialog.get(0,"event_timestamp"),metadata.dump()});
}
}

