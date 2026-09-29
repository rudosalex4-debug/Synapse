#include "moderation.hpp"
#include "workflow.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <set>
#include <vector>

namespace maxhelp {
namespace {
[[noreturn]] void invalid(){throw AppError(400,"INVALID_MODERATION_REQUEST","Проверьте параметры обращения.");}
[[noreturn]] void missing(){throw AppError(404,"REPORT_NOT_FOUND","Жалоба не найдена.");}
void require_uuid(const std::string& value){
 if(value.size()!=36)invalid();
 for(std::size_t i=0;i<value.size();++i){
  if(i==8||i==13||i==18||i==23){if(value[i]!='-')invalid();}
  else if(!((value[i]>='0'&&value[i]<='9')||(value[i]>='a'&&value[i]<='f')))invalid();
 }
}
long long number(const std::string& value){
 long long result=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
 if(value.empty()||parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||result<1)invalid();
 return result;
}
std::string param(const std::map<std::string,std::string>& query,const std::string& key,const std::string& fallback){
 auto it=query.find(key);return it==query.end()?fallback:it->second;
}
void allowed_query(const std::map<std::string,std::string>& query,std::initializer_list<std::string> keys){
 for(const auto& [key,value]:query){(void)value;if(std::find(keys.begin(),keys.end(),key)==keys.end())invalid();}
}
Json nullable(const Result& rows,int i,const char* key){return rows.is_null(i,key)?Json(nullptr):Json(rows.get(i,key));}
const std::string columns=
 "SELECT r.*,c.status AS conversation_status,c.author_id,c.helper_id,q.id AS request_id,q.title AS request_title,"
 "reporter.display_name AS reporter_name,other.display_name AS target_name,other.id AS target_id,"
 "to_char(r.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_iso,"
 "to_char(r.resolved_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS resolved_iso "
 "FROM safety_reports r JOIN conversations c ON c.id=r.conversation_id JOIN help_requests q ON q.id=c.request_id "
 "JOIN users reporter ON reporter.id=r.reporter_id "
 "JOIN users other ON other.id=CASE WHEN r.reporter_id=c.author_id THEN c.helper_id ELSE c.author_id END ";
Result read_report(Db& db,const std::string& id,bool lock=false){
 auto rows=db.exec(columns+"WHERE r.id=$1::uuid"+(lock?" FOR UPDATE OF r":""),{id});
 if(!rows.size())missing();return rows;
}
Json report_value(const Result& rows,int i,bool detail){
 Json value={{"id",rows.get(i,"id")},{"status",rows.get(i,"status")},{"revision",std::stoll(rows.get(i,"revision"))},
  {"category",rows.get(i,"category")},{"createdAt",rows.get(i,"created_iso")},
  {"reporterName",rows.get(i,"reporter_name")},{"targetName",rows.get(i,"target_name")},
  {"requestTitle",rows.get(i,"request_title")},{"resolution",nullable(rows,i,"resolution")},
  {"resolvedAt",nullable(rows,i,"resolved_iso")}};
 if(detail){value["text"]=rows.get(i,"text");value["conversationId"]=rows.get(i,"conversation_id");
  value["conversationStatus"]=rows.get(i,"conversation_status");value["note"]=nullable(rows,i,"resolution_note");}
 return value;
}
void audit(Db& db,const std::string& actor,const std::string& report,const std::string& action,
           const std::optional<std::string>& decision={},const std::optional<std::string>& first={},
           const std::optional<std::string>& last={}){
 db.exec("INSERT INTO moderation_audit(moderator_id,report_id,action,decision,first_sequence,last_sequence) "
         "VALUES($1::uuid,$2::uuid,$3,$4,$5::bigint,$6::bigint)",{actor,report,action,decision,first,last});
}
std::string review_note(const Json& body){
 if(!body.at("note").is_string())invalid();
 const auto note=body.at("note").get<std::string>();
 // JSON parser already validates UTF-8; count Unicode code points, not bytes.
 std::size_t length=0;bool visible=false;
 for(std::size_t i=0;i<note.size();){
  const auto lead=static_cast<unsigned char>(note[i]);std::size_t width=1;std::uint32_t point=lead;
  if(lead>=0xf0){width=4;point=lead&7;}else if(lead>=0xe0){width=3;point=lead&15;}else if(lead>=0xc0){width=2;point=lead&31;}
  if(i+width>note.size())invalid();
  for(std::size_t j=1;j<width;++j)point=(point<<6)|(static_cast<unsigned char>(note[i+j])&63);
  if((point<32&&point!=9&&point!=10&&point!=13)||(point>=127&&point<=159))invalid();
  const bool space=point==32||point==9||point==10||point==13||point==0xa0||point==0x1680||
   (point>=0x2000&&point<=0x200b)||point==0x2028||point==0x2029||point==0x202f||point==0x205f||point==0x3000||point==0xfeff;
  visible=visible||!space;++length;i+=width;
 }
 if(length<1||length>1000||!visible)invalid();return note;
}
}

bool can_moderate(Db& db,const Config& config,const std::string& actor){
 auto user=db.exec("SELECT max_user_id,provenance FROM users WHERE id=$1::uuid",{actor});
 return user.size()&&!user.is_null(0,"max_user_id")&&user.get(0,"provenance")!="demo"&&
  config.moderator_max_ids.contains(user.get(0,"max_user_id"))&&pilot_allows(config,user.get(0,"max_user_id"));
}

Json moderation(Db& db,const Config& config,const Json& catalog,const std::string& actor,
 const std::string& method,const std::string& path,const Json& body,const std::string& key,
 const std::map<std::string,std::string>& query){
 const bool allowed=can_moderate(db,config,actor);
 if(path=="/api/moderation/access"&&method=="GET"){
  allowed_query(query,{});return {{"canModerate",allowed}};
 }
 if(!allowed)throw AppError(403,"MODERATOR_REQUIRED","Этот раздел доступен только назначенному ответственному.");
 const std::string base="/api/moderation/reports";
 if(path==base&&method=="GET"){
  allowed_query(query,{"status","cursor"});const auto status=param(query,"status","new");
  if(status!="new"&&status!="resolved"&&status!="all")invalid();
  const auto before=param(query,"cursor","9223372036854775807");number(before);
  auto rows=db.exec(columns+"WHERE ($1='all' OR r.status=$1) AND r.queue_id<$2::bigint ORDER BY r.queue_id DESC LIMIT 31",{status,before});
  Json items=Json::array();for(int i=0;i<std::min(rows.size(),30);++i)items.push_back(report_value(rows,i,false));
  return {{"items",items},{"nextCursor",rows.size()>30?Json(rows.get(29,"queue_id")):Json(nullptr)}};
 }
 if(!path.starts_with(base+"/"))missing();
 const auto tail=path.substr(base.size()+1);const auto slash=tail.find('/');
 const auto id=tail.substr(0,slash),action=slash==std::string::npos?std::string{}:tail.substr(slash+1);
 require_uuid(id);
 if(method=="GET"&&action.empty()){
  allowed_query(query,{});Transaction tx(db);auto rows=read_report(db,id);
  audit(db,actor,id,"view_report");auto result=report_value(rows,0,true);tx.commit();return result;
 }
 if(method=="GET"&&action=="messages"){
  allowed_query(query,{"beforeSequence"});const auto before=param(query,"beforeSequence","9223372036854775807");number(before);
  Transaction tx(db);auto report=read_report(db,id);
  auto rows=db.exec("SELECT m.sequence,m.text,u.display_name AS sender_name,"
   "to_char(m.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_iso "
   "FROM conversation_messages m JOIN users u ON u.id=m.sender_id WHERE m.conversation_id=$1::uuid AND m.sequence<$2::bigint "
   "ORDER BY m.sequence DESC LIMIT 31",{report.get(0,"conversation_id"),before});
  const int count=std::min(rows.size(),30);Json items=Json::array();
  for(int i=count-1;i>=0;--i)items.push_back({{"sequence",std::stoll(rows.get(i,"sequence"))},{"text",rows.get(i,"text")},
    {"senderName",rows.get(i,"sender_name")},{"createdAt",rows.get(i,"created_iso")}});
  audit(db,actor,id,"view_messages",{},count?std::optional(rows.get(count-1,"sequence")):std::nullopt,
    count?std::optional(rows.get(0,"sequence")):std::nullopt);
  Json result={{"items",items},{"nextBeforeSequence",rows.size()>30?Json(std::stoll(rows.get(29,"sequence"))):Json(nullptr)}};
  tx.commit();return result;
 }
 if(method!="POST"||action!="decision")missing();
 allowed_query(query,{});require_uuid(key);
 if(!body.is_object()||body.size()!=3||!body.contains("expectedRevision")||!body.contains("decision")||!body.contains("note")||
    !body.at("expectedRevision").is_number_integer()||!body.at("decision").is_string())invalid();
 if(body.at("expectedRevision")<0||body.at("expectedRevision")>9007199254740991LL)invalid();
 const auto decision=body.at("decision").get<std::string>(),note=review_note(body);
 if(decision!="dismiss"&&decision!="close_conversation"&&decision!="block_pair")invalid();
 Transaction tx(db);auto initial=read_report(db,id);
 const auto reporter=initial.get(0,"reporter_id"),target=initial.get(0,"target_id");
 if(actor==reporter||actor==target)throw AppError(403,"MODERATION_CONFLICT","Решение по своему диалогу должен принять другой ответственный.");
 // Match the workflow lock order: users first, then reports/requests/conversations.
 std::set<std::string> users{actor,reporter,target};
 for(const auto& user:users)db.exec("SELECT id FROM users WHERE id=$1::uuid FOR UPDATE",{user});
 auto report=read_report(db,id,true);const auto hash=sha256(body.dump());
 auto previous=db.exec("SELECT payload_hash,result FROM workflow_idempotency WHERE actor_id=$1::uuid AND method='POST' AND route=$2 AND key=$3::uuid",{actor,path,key});
 if(previous.size()){
  if(previous.get(0,"payload_hash")!=hash)throw AppError(409,"IDEMPOTENCY_CONFLICT","Этот ключ уже использован с другим решением.");
  auto result=Json::parse(previous.get(0,"result"));tx.commit();return result;
 }
 if(report.get(0,"status")!="new"||std::stoll(report.get(0,"revision"))!=body.at("expectedRevision").get<long long>())
  throw AppError(409,"REVISION_CONFLICT","Жалоба уже изменена. Загрузите актуальное решение.");
 if(decision=="block_pair"){
  auto active=db.exec("SELECT id FROM conversations WHERE status='active' AND ((author_id=$1::uuid AND helper_id=$2::uuid) "
                     "OR (author_id=$2::uuid AND helper_id=$1::uuid))",{reporter,target});
  // Use the same block semantics as the user workflow, inside this audited transaction.
  workflow(db,config,catalog,reporter,"POST","/api/blocks",{{"userId",target}},uuid(),"",{},false);
  for(int i=0;i<active.size();++i)db.exec("UPDATE conversations SET moderation_closed=true WHERE id=$1::uuid",{active.get(i,"id")});
 }else if(decision=="close_conversation"){
  db.exec("SELECT id FROM help_requests WHERE id=$1::uuid FOR UPDATE",{report.get(0,"request_id")});
  auto changed=db.exec("UPDATE conversations SET status='closed',closed_at=now(),outcome='no_result',moderation_closed=true "
   "WHERE id=$1::uuid AND status='active' RETURNING id",{report.get(0,"conversation_id")});
  if(changed.size())db.exec("UPDATE help_requests SET status='closed_unresolved',revision=revision+1,updated_at=now() WHERE id=$1::uuid",{report.get(0,"request_id")});
 }
 db.exec("UPDATE safety_reports SET status='resolved',revision=revision+1,resolution=$2,resolution_note=$3,resolved_by=$4::uuid,resolved_at=now() WHERE id=$1::uuid",{id,decision,note,actor});
 audit(db,actor,id,"decision",decision);
 auto result=report_value(read_report(db,id),0,true);
 db.exec("INSERT INTO workflow_idempotency(actor_id,method,route,key,payload_hash,result) VALUES($1::uuid,'POST',$2,$3::uuid,$4,$5::jsonb)",{actor,path,key,hash,result.dump()});
 tx.commit();return result;
}
}
