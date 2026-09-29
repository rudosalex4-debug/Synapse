#include "workflow.hpp"
#include "matching.hpp"
#include "notifications.hpp"
#include "community_rules.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace maxhelp {
namespace {
[[noreturn]] void bad(const std::string& message) { throw AppError(400,"INVALID_REQUEST",message); }
[[noreturn]] void missing() { throw AppError(404,"NOT_FOUND","Объект не найден или недоступен."); }
[[noreturn]] void conflict(const std::string& message) { throw AppError(409,"WORKFLOW_CONFLICT",message); }
void require_active_topic(const Json& catalog,const Json& req){
 const auto id=req.at("topicId").get<std::string>();
 for(const auto& item:catalog.at("topics"))if(item.at("id")==id&&item.at("active").get<bool>()&&item.at("level").get<int>()>=2)return;
 throw AppError(409,"TOPIC_ARCHIVED","Тема сохранена в истории и временно недоступна. Для нового поиска выберите тему из образовательных траекторий или карьеры.");
}
void fields(const Json& body,std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional={}) {
 if(!body.is_object())bad("Ожидается JSON-объект.");
 for(auto field:required)if(!body.contains(std::string(field)))bad("Не заполнено поле: "+std::string(field));
 for(auto it=body.begin();it!=body.end();++it)
  if(it.value().is_null()||(std::find(required.begin(),required.end(),it.key())==required.end()&&
     std::find(optional.begin(),optional.end(),it.key())==optional.end()))bad("Недопустимое поле: "+it.key());
}
bool valid_uuid(const std::string& value) {
 if(value.size()!=36)return false;
 for(std::size_t i=0;i<value.size();++i){
  if(i==8||i==13||i==18||i==23){if(value[i]!='-')return false;}
  else if(!((value[i]>='0'&&value[i]<='9')||(value[i]>='a'&&value[i]<='f')))return false;
 }return true;
}
void require_uuid(const std::string& value){if(!valid_uuid(value))bad("Ожидается UUID в стандартном формате.");}
std::string text(const Json& body,const char* key,std::size_t minimum,std::size_t maximum,bool multiline=true){
 if(!body.at(key).is_string())bad(std::string(key)+": ожидается строка.");
 auto value=body.at(key).get<std::string>();std::size_t count=0;bool visible=false;
 for(std::size_t i=0;i<value.size();){
  auto lead=static_cast<unsigned char>(value[i]);std::uint32_t point=0,lowest=0;std::size_t width=0;
  if(lead<128){point=lead;width=1;}
  else if(lead>=0xc2&&lead<=0xdf){point=lead&0x1f;width=2;lowest=0x80;}
  else if(lead>=0xe0&&lead<=0xef){point=lead&0x0f;width=3;lowest=0x800;}
  else if(lead>=0xf0&&lead<=0xf4){point=lead&7;width=4;lowest=0x10000;}
  else bad("Некорректная кодировка текста.");
  if(i+width>value.size())bad("Некорректная кодировка текста.");
  for(std::size_t j=1;j<width;++j){auto next=static_cast<unsigned char>(value[i+j]);
   if((next&0xc0)!=0x80)bad("Некорректная кодировка текста.");
   point=(point<<6)|(next&0x3f);}
  if(point<lowest||point>0x10ffff||(point>=0xd800&&point<=0xdfff))bad("Некорректная кодировка текста.");
  if((point<32&&!(multiline&&(point==9||point==10||point==13)))||(point>=127&&point<=159))
   bad("Управляющие символы недопустимы.");
  bool space=point==32||point==9||point==10||point==13||point==0xa0||point==0x1680||
   (point>=0x2000&&point<=0x200a)||point==0x2028||point==0x2029||point==0x202f||
   point==0x205f||point==0x3000||point==0xfeff||point==0x200b;
  visible=visible||!space;++count;i+=width;
 }
 if(count<minimum||count>maximum||(minimum>0&&!visible))bad(std::string(key)+": проверьте длину и содержание.");
 return value;
}
Json nullable(const Result& rows,int i,const char* field){return rows.is_null(i,field)?Json(nullptr):Json(rows.get(i,field));}
std::vector<std::string> parts(const std::string& path){
 std::vector<std::string> result;std::size_t start=1;
 while(start<=path.size()){auto end=path.find('/',start);result.push_back(path.substr(start,end==std::string::npos?end:end-start));
  if(end==std::string::npos)break;
  start=end+1;}return result;
}
long long number(const std::string& value,long long low,long long high){
 long long result=0;auto conversion=std::from_chars(value.data(),value.data()+value.size(),result);
 if(value.empty()||conversion.ec!=std::errc{}||conversion.ptr!=value.data()+value.size()||result<low||result>high)
  bad("Некорректный параметр страницы.");
 return result;
}
std::string query_value(const std::map<std::string,std::string>& query,const std::string& key,const std::string& fallback){
 auto it=query.find(key);return it==query.end()?fallback:it->second;
}
void query_fields(const std::map<std::string,std::string>& query,std::initializer_list<std::string_view> allowed){
 for(const auto& [key,value]:query){(void)value;if(std::find(allowed.begin(),allowed.end(),key)==allowed.end())bad("Неизвестный параметр страницы.");}
}
// Generated cursors carry a fixed UTC timestamp and UUID. Validate dates before SQL casts.
std::pair<std::string,std::string> cursor(const std::string& value){
 if(value.empty())return {"",""};
 if(value.size()!=64||value[27]!='|')bad("Некорректный курсор.");
 auto date=value.substr(0,27),id=value.substr(28);require_uuid(id);
 if(date[4]!='-'||date[7]!='-'||date[10]!='T'||date[13]!=':'||date[16]!=':'||date[19]!='.'||date[26]!='Z')bad("Некорректный курсор.");
 int year=static_cast<int>(number(date.substr(0,4),1,9999));
 unsigned month=static_cast<unsigned>(number(date.substr(5,2),1,12)),day=static_cast<unsigned>(number(date.substr(8,2),1,31));
 if(!std::chrono::year_month_day{std::chrono::year(year),std::chrono::month(month),std::chrono::day(day)}.ok())bad("Некорректный курсор.");
 number(date.substr(11,2),0,23);number(date.substr(14,2),0,59);number(date.substr(17,2),0,59);number(date.substr(20,6),0,999999);
 return {date,id};
}
const std::string request_columns=
 "SELECT r.*,u.display_name AS author_name,CASE WHEN r.status='open' AND r.expires_at<=now() THEN 'expired' ELSE r.status END AS current_status,"
 "to_char(r.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_iso,"
 "to_char(r.updated_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS updated_iso "
 "FROM help_requests r JOIN users u ON u.id=r.author_id ";
Json request_value(const Result& rows,int i){
 return {{"id",rows.get(i,"id")},{"authorId",rows.get(i,"author_id")},{"authorName",rows.get(i,"author_name")},
  {"title",rows.get(i,"title")},{"body",rows.get(i,"body")},{"learningGoal",rows.get(i,"learning_goal")},
  {"attempt",nullable(rows,i,"attempt")},{"topicId",rows.get(i,"topic_id")},{"facets",Json::parse(rows.get(i,"facets"))},
  {"requiredFacets",Json::parse(rows.get(i,"required_facets"))},{"desiredExperience",nullable(rows,i,"desired_experience")},
  {"taxonomyVersion",rows.get(i,"taxonomy_version")},{"status",rows.get(i,"current_status")},
  {"revision",std::stoll(rows.get(i,"revision"))},{"createdAt",rows.get(i,"created_iso")},{"updatedAt",rows.get(i,"updated_iso")}};
}
Json request(Db& db,const std::string& id,bool lock=false){
 auto rows=db.exec(request_columns+"WHERE r.id=$1::uuid"+(lock?" FOR UPDATE OF r":""),{id});
 if(!rows.size())missing();
 return request_value(rows,0);
}
const std::string offer_columns=
 "SELECT o.*,u.display_name AS helper_name,to_char(o.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_iso "
 "FROM help_offers o JOIN users u ON u.id=o.helper_id ";
Json offer_value(const Result& rows,int i){
 auto snapshot=Json::parse(rows.get(i,"competency_snapshot"));
 return {{"id",rows.get(i,"id")},{"requestId",rows.get(i,"request_id")},{"helperId",rows.get(i,"helper_id")},
  {"helperName",rows.get(i,"helper_name")},{"message",rows.get(i,"message")},{"status",rows.get(i,"status")},
  {"createdAt",rows.get(i,"created_iso")},{"competency",snapshot},{"explanation",snapshot.value("explanation",Json(nullptr))},
  {"score",std::stod(rows.get(i,"score"))},{"narrower",rows.get(i,"narrower")=="t"}};
}
Json offer(Db& db,const std::string& id){
 auto rows=db.exec(offer_columns+"WHERE o.id=$1::uuid",{id});if(!rows.size())missing();
 return offer_value(rows,0);
}
Json decorate_request(Db& db,Json value,const std::string& actor){
 auto id=value.at("id").get<std::string>();
 auto offers=db.exec(offer_columns+"WHERE o.request_id=$1::uuid AND o.helper_id=$2::uuid",{id,actor});
 value["myOffer"]=offers.size()?offer_value(offers,0):Json(nullptr);
 auto thread=db.exec("SELECT id FROM conversations WHERE request_id=$1::uuid AND (author_id=$2::uuid OR helper_id=$2::uuid)",{id,actor});
 value["conversationId"]=thread.size()?Json(thread.get(0,"id")):Json(nullptr);return value;
}
const std::string conversation_columns=
 "SELECT c.*,r.title,a.display_name AS author_name,h.display_name AS helper_name,"
 "to_char(c.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_iso,"
 "to_char(c.closed_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS closed_iso "
 "FROM conversations c JOIN help_requests r ON r.id=c.request_id JOIN users a ON a.id=c.author_id JOIN users h ON h.id=c.helper_id ";
Json conversation_value(const Result& rows,int i,const std::string& actor){
 bool author=rows.get(i,"author_id")==actor;if(!author&&rows.get(i,"helper_id")!=actor)missing();
 return {{"id",rows.get(i,"id")},{"requestId",rows.get(i,"request_id")},{"title",rows.get(i,"title")},
  {"authorId",rows.get(i,"author_id")},{"helperId",rows.get(i,"helper_id")},
  {"otherUserId",rows.get(i,author?"helper_id":"author_id")},{"otherDisplayName",rows.get(i,author?"helper_name":"author_name")},
  {"status",rows.get(i,"status")},{"createdAt",rows.get(i,"created_iso")},{"closedAt",nullable(rows,i,"closed_iso")},
  {"moderationClosed",rows.get(i,"moderation_closed")=="t"},{"outcome",nullable(rows,i,"outcome")},{"comment",nullable(rows,i,"comment")},{"nextStep",nullable(rows,i,"next_step")}};
}
Json conversation(Db& db,const std::string& id,const std::string& actor,bool lock=false){
 auto rows=db.exec(conversation_columns+"WHERE c.id=$1::uuid"+(lock?" FOR UPDATE OF c":""),{id});
 if(!rows.size())missing();
 return conversation_value(rows,0,actor);
}
void lock_users(Db& db,const std::set<std::string>& users,bool mutation){
 Json ids=Json::array();for(const auto& id:users)ids.push_back(id);
 auto rows=db.exec("SELECT id FROM users WHERE id IN (SELECT value::uuid FROM jsonb_array_elements_text($1::jsonb)) ORDER BY id "+
  std::string(mutation?"FOR UPDATE":"FOR SHARE"),{ids.dump()});
 if(rows.size()!=static_cast<int>(users.size()))missing();
}
bool blocked(Db& db,const std::string& a,const std::string& b){
 return db.exec("SELECT 1 FROM user_blocks WHERE (blocker_id=$1::uuid AND blocked_id=$2::uuid) OR "
  "(blocker_id=$2::uuid AND blocked_id=$1::uuid) LIMIT 1",{a,b}).size()!=0;
}
void allow_pair(Db& db,const std::string& a,const std::string& b){
 if(blocked(db,a,b))throw AppError(403,"INTERACTION_BLOCKED","Взаимодействие с участником недоступно.");
}
void own(const Json& object,const char* field,const std::string& actor){if(object.at(field)!=actor)missing();}
void empty_body(const Json& body){fields(body,{});}
Json validate_request(const Json& body,const Json& catalog){
 fields(body,{"title","body","learningGoal","topicId","facets","requiredFacets","taxonomyVersion"},{"attempt","desiredExperience"});
 auto title=text(body,"title",1,120,false),description=text(body,"body",30,2000),goal=text(body,"learningGoal",1,30,false);
 if(goal!="understand"&&goal!="practice"&&goal!="troubleshoot"&&goal!="learning_path")bad("Неизвестная учебная цель.");
 auto topic=text(body,"topicId",1,200,false),version=text(body,"taxonomyVersion",1,100,false);
 if(version!=catalog.at("version").get<std::string>())throw AppError(409,"CATALOG_CHANGED","Обновите каталог перед сохранением.");
 auto experience=body.contains("desiredExperience")?text(body,"desiredExperience",1,30,false):std::string("self_study");
 Json skill={{"topicId",topic},{"facets",body.at("facets")},{"experienceKind",experience},{"description",""},{"evidenceVisibility","private"}};
 validate_profile({{"displayName","validation"},{"bio",""},{"availableToHelp",true},{"maxActiveConversations",2},
  {"competencies",Json::array({skill})}},catalog);
 if(!body.at("requiredFacets").is_array())bad("requiredFacets: ожидается массив.");
 std::set<std::string> required;
 for(const auto& value:body.at("requiredFacets"))
  if(!value.is_string()||!required.insert(value.get<std::string>()).second||!body.at("facets").contains(value.get<std::string>()))
   bad("Обязательное уточнение должно иметь выбранные значения.");
 Json result=body;result["title"]=title;result["body"]=description;
 result["attempt"]=body.contains("attempt")?Json(text(body,"attempt",0,1000)):Json(nullptr);
 result["desiredExperience"]=body.contains("desiredExperience")?Json(experience):Json(nullptr);return result;
}
MatchFacets match_facets(const Json& facets){
 MatchFacets result;for(auto it=facets.begin();it!=facets.end();++it)for(const auto& value:it.value())result[it.key()].insert(value.get<std::string>());
 return result;
}
MatchQuery match_query(const Json& req){
 MatchQuery query;query.author_id=req.at("authorId").get<std::string>();query.topic_id=req.at("topicId").get<std::string>();
 query.facets=match_facets(req.at("facets"));
 if(!req.at("desiredExperience").is_null())query.desired_experience=req.at("desiredExperience").get<std::string>();
 for(const auto& required:req.at("requiredFacets"))query.required_facets.insert(required.get<std::string>());
 query.limit=1;return query;
}
Json match_explanation(const Json& req,const Json& snapshot,bool narrower){
 Json matched=Json::array(),missing_optional=Json::array();const auto own_facets=match_facets(snapshot.at("facets"));
 std::set<std::string> required;for(const auto& value:req.at("requiredFacets"))required.insert(value.get<std::string>());
 for(auto it=req.at("facets").begin();it!=req.at("facets").end();++it){
  bool hit=false;auto existing=own_facets.find(it.key());
  if(existing!=own_facets.end())for(const auto& value:it.value())if(existing->second.contains(value.get<std::string>())){hit=true;break;}
  if(hit)matched.push_back(it.key());else if(!required.contains(it.key()))missing_optional.push_back(it.key());
 }
 return {{"topicRelation",narrower?"narrower":"exact"},{"matchedFacets",matched},{"missingOptionalFacets",missing_optional},
  {"experience",req.at("desiredExperience").is_null()?"not_requested":req.at("desiredExperience")==snapshot.at("experienceKind")?"matched":"different"},
  {"basis","self_declared"}};
}
Json request_reach(Db& db,const Config& config,const Json& catalog,const Json& req){
 require_active_topic(catalog,req);
 // The caller has checked ownership. Candidate retrieval uses the topic index,
 // then the same scorer as the helper feed; no per-person repository calls.
 std::map<std::string,std::string> parents;std::map<std::string,int> levels;
 for(const auto& topic:catalog.at("topics"))if(topic.at("active").get<bool>()){
  const auto id=topic.at("id").get<std::string>();parents[id]=topic.at("parent_id").is_null()?"":topic.at("parent_id").get<std::string>();
  levels[id]=topic.at("level").get<int>();
 }
 const auto request_topic=req.at("topicId").get<std::string>();Json topics=Json::array(),parent=nullptr;
 for(const auto& [id,unused]:parents){(void)unused;if(levels.at(id)<2)continue;
  for(auto current=id;!current.empty()&&parents.contains(current);current=parents.at(current))
   if(current==request_topic){topics.push_back(id);break;}
 }
 if(parents.contains(request_topic)){const auto& id=parents.at(request_topic);
  if(levels.contains(id)&&levels.at(id)>=2)parent=id;}
 auto rows=db.exec("SELECT c.id,c.user_id,c.topic_id,c.facets,c.experience_kind,u.max_active_conversations,capacity.load "
  "FROM competencies c JOIN users u ON u.id=c.user_id "
  "CROSS JOIN LATERAL (SELECT count(*) AS load FROM conversations v WHERE v.helper_id=u.id AND v.status='active') capacity "
  "WHERE c.topic_id IN (SELECT value FROM jsonb_array_elements_text($1::jsonb)) AND c.user_id<>$2::uuid "
  "AND u.accepted_rules_version=$5 AND u.available_to_help AND capacity.load<u.max_active_conversations "
  "AND (NOT $3::boolean OR u.max_user_id IN (SELECT value FROM jsonb_array_elements_text($4::jsonb))) "
  "AND NOT EXISTS(SELECT 1 FROM user_blocks b WHERE (b.blocker_id=$2::uuid AND b.blocked_id=c.user_id) "
  "OR (b.blocked_id=$2::uuid AND b.blocker_id=c.user_id)) "
  "ORDER BY c.topic_id,c.user_id,c.id LIMIT 1001",
  {topics.dump(),req.at("authorId").get<std::string>(),config.pilot_mode?"true":"false",Json(config.pilot_allowed_max_ids).dump(),community_rules_version});
 std::vector<MatchCompetency> candidates;candidates.reserve(std::min(rows.size(),1000));
 for(int i=0;i<rows.size()&&i<1000;++i){MatchCompetency skill;
  skill.id=rows.get(i,"id");skill.user_id=rows.get(i,"user_id");skill.topic_id=rows.get(i,"topic_id");
  skill.facets=match_facets(Json::parse(rows.get(i,"facets")));skill.experience=rows.get(i,"experience_kind");
  skill.load=static_cast<unsigned>(std::stoul(rows.get(i,"load")));skill.capacity=static_cast<unsigned>(std::stoul(rows.get(i,"max_active_conversations")));
  candidates.push_back(std::move(skill));}
 const bool limited=rows.size()>1000;
 bool available=false;
 if(levels.contains(request_topic)&&levels.at(request_topic)>=2){MatchingIndex index(catalog,std::move(candidates));available=!index.search(match_query(req)).results.empty();}
 Json suggestions=Json::array();
 if(req.at("attempt").is_null()||req.at("attempt").get<std::string>().empty())suggestions.push_back("add_attempt");
 if(!available){if(!req.at("requiredFacets").empty())suggestions.push_back("review_required_facets");if(!parent.is_null())suggestions.push_back("choose_parent_topic");}
 auto checked=db.exec("SELECT to_char(clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS checked");
 return {{"requestId",req.at("id")},{"requestRevision",req.at("revision")},{"status",available?"available":limited?"limited":"no_match"},
  {"sampleLimited",limited},{"checkedAt",checked.get(0,"checked")},{"suggestions",suggestions},{"parentTopicId",parent}};
}
class HelperIndex {
 std::unique_ptr<MatchingIndex> index_;std::map<std::string,Json> snapshots_;
public:
 std::set<std::string> request_topics;
 HelperIndex(Db& db,const Json& catalog,const std::string& actor,const Config& config){
  // Only this helper's <=30 rows; profile changes serialize on the user lock.
  auto skills=db.exec("SELECT c.id,c.user_id,c.topic_id,c.facets,c.experience_kind,c.description,c.evidence_status,c.provenance,"
   "u.available_to_help,u.max_active_conversations,u.max_user_id,"
   "(SELECT count(*) FROM conversations v WHERE v.helper_id=u.id AND v.status='active') AS load "
   "FROM competencies c JOIN users u ON u.id=c.user_id WHERE c.user_id=$1::uuid AND u.accepted_rules_version=$2 ORDER BY c.id",{actor,community_rules_version});
  std::map<std::string,std::string> parents;std::set<std::string> selectable;
  for(const auto& topic:catalog.at("topics")){if(!topic.at("active").get<bool>())continue;
   auto id=topic.at("id").get<std::string>();parents[id]=topic.at("parent_id").is_null()?"":topic.at("parent_id").get<std::string>();
   if(topic.at("level").get<int>()>=2)selectable.insert(id);}
  std::vector<MatchCompetency> values;
  for(int i=0;i<skills.size();++i){
   MatchCompetency skill;skill.id=skills.get(i,"id");skill.user_id=actor;skill.topic_id=skills.get(i,"topic_id");
   skill.experience=skills.get(i,"experience_kind");skill.facets=match_facets(Json::parse(skills.get(i,"facets")));
   skill.available=skills.get(i,"available_to_help")=="t";
   skill.capacity=static_cast<unsigned>(std::stoul(skills.get(i,"max_active_conversations")));
   skill.load=static_cast<unsigned>(std::stoul(skills.get(i,"load")));
   if(!selectable.contains(skill.topic_id)||!skill.available||skill.load>=skill.capacity||!pilot_allows(config,skills.get(i,"max_user_id")))continue;
   snapshots_[skill.id]={{"id",skill.id},{"topicId",skill.topic_id},{"description",skills.get(i,"description")},
    {"experienceKind",skill.experience},{"facets",Json::parse(skills.get(i,"facets"))},{"evidenceStatus",skills.get(i,"evidence_status")},{"provenance",skills.get(i,"provenance")}};
   for(auto topic=skill.topic_id;!topic.empty()&&parents.contains(topic);topic=parents.at(topic))
    if(selectable.contains(topic))request_topics.insert(topic);
   values.push_back(std::move(skill));
  }index_=std::make_unique<MatchingIndex>(catalog,std::move(values));
 }
 std::optional<Json> match(const Json& req)const{
  if(!request_topics.contains(req.at("topicId").get<std::string>()))return std::nullopt;
  auto page=index_->search(match_query(req));if(page.results.empty())return std::nullopt;
  const auto& result=page.results.front();return Json{{"score",result.score},{"competencyId",result.competency_id},
   {"topicId",snapshots_.at(result.competency_id).at("topicId")},{"narrower",result.narrower},
   {"explanation",match_explanation(req,snapshots_.at(result.competency_id),result.narrower)}};
 }
 Json snapshot(const Json& match)const{auto result=snapshots_.at(match.at("competencyId").get<std::string>());result["explanation"]=match.at("explanation");return result;}
};
void readable_request(Db& db,const Json& catalog,Json& req,const std::string& actor,const Config& config){
 if(req.at("authorId")==actor)return;
 auto selected=db.exec("SELECT 1 FROM conversations WHERE request_id=$1::uuid AND helper_id=$2::uuid",
  {req.at("id").get<std::string>(),actor});
 if(selected.size())return; // Prior participants retain read-only history after a block.
 allow_pair(db,actor,req.at("authorId").get<std::string>());
 if(req.at("status")!="open")missing();
 auto author=db.exec("SELECT max_user_id,accepted_rules_version FROM users WHERE id=$1::uuid",{req.at("authorId").get<std::string>()});
 if(!author.size()||author.get(0,"accepted_rules_version")!=community_rules_version||!pilot_allows(config,author.get(0,"max_user_id")))missing();
 HelperIndex index(db,catalog,actor,config);auto match=index.match(req);if(!match)missing();req["match"]=*match;
}
Json message_value(const Result& rows,int i){
 return {{"id",rows.get(i,"id")},{"conversationId",rows.get(i,"conversation_id")},{"senderId",rows.get(i,"sender_id")},
  {"senderName",rows.get(i,"sender_name")},{"sequence",std::stoll(rows.get(i,"sequence"))},{"text",rows.get(i,"text")},
  {"createdAt",rows.get(i,"created_iso")},{"clientMessageId",rows.get(i,"client_message_id")}};
}
const std::string message_columns=
 "SELECT m.*,u.display_name AS sender_name,to_char(m.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_iso "
 "FROM conversation_messages m JOIN users u ON u.id=m.sender_id ";
} // namespace


Json workflow(Db& db,const Config& config,const Json& catalog,const std::string& actor,
 const std::string& method,const std::string& path,const Json& body,
 const std::string& idempotency_key,const std::string& if_match,
 const std::map<std::string,std::string>& query,bool own_transaction){
 require_uuid(actor);auto route=parts(path);
 if(route.size()<2||route[0]!="api"||route.size()>4)missing();
 const auto& area=route[1];
 std::string id=route.size()>=3?route[2]:"",action=route.size()>=4?route[3]:"";
 if(route.size()>=3)require_uuid(id);
 const bool requests=area=="requests",offers=area=="offers",chats=area=="conversations",
  blocks=area=="blocks",reports=area=="reports",mutation=method!="GET";
 const bool valid_route=
  (requests&&id.empty()&&(method=="GET"||method=="POST"))||
  (requests&&!id.empty()&&action.empty()&&(method=="GET"||method=="PUT"))||
  (requests&&!id.empty()&&action=="offers"&&(method=="GET"||method=="POST"))||
  (requests&&!id.empty()&&action=="reach"&&method=="GET")||
  (requests&&!id.empty()&&(action=="publish"||action=="cancel")&&method=="POST")||
  (offers&&!id.empty()&&(action=="withdraw"||action=="accept")&&method=="POST")||
  (chats&&id.empty()&&method=="GET")||
  (chats&&!id.empty()&&action=="messages"&&(method=="GET"||method=="POST"))||
  (chats&&!id.empty()&&action=="close"&&method=="POST")||
  ((blocks||reports)&&id.empty()&&method=="POST");
 if(!valid_route)missing();
 const bool safety_action=blocks||reports||action=="cancel"||action=="withdraw"||action=="close";
 if(mutation&&!safety_action)require_current_rules(db,actor);
 if(method=="GET"&&requests&&id.empty())query_fields(query,{"scope","after","limit"});
 else if(method=="GET"&&chats&&action=="messages")query_fields(query,{"after","limit"});
 else query_fields(query,{});
 const bool use_key=method=="POST"&&!(chats&&action=="messages");
 if(use_key){if(idempotency_key.empty())throw AppError(400,"IDEMPOTENCY_KEY_REQUIRED","Для операции необходим Idempotency-Key.");
  require_uuid(idempotency_key);}
 // Discover first, lock users in sorted order, then re-read mutable resources.
 std::unique_ptr<Transaction> transaction;if(own_transaction)transaction=std::make_unique<Transaction>(db);
 std::set<std::string> users{actor};Json req,selected_offer,thread;std::string target;
 if(requests&&!id.empty()){req=request(db,id);if(action=="reach")own(req,"authorId",actor);users.insert(req.at("authorId").get<std::string>());}
 else if(offers){selected_offer=offer(db,id);req=request(db,selected_offer.at("requestId").get<std::string>());
  if(action=="accept")own(req,"authorId",actor);else own(selected_offer,"helperId",actor);
  users.insert(req.at("authorId").get<std::string>());users.insert(selected_offer.at("helperId").get<std::string>());}
 else if(chats&&!id.empty()){thread=conversation(db,id,actor);users.insert(thread.at("authorId").get<std::string>());
  users.insert(thread.at("helperId").get<std::string>());}
 else if(blocks){fields(body,{"userId"});target=text(body,"userId",36,36,false);require_uuid(target);
  if(target==actor)bad("Нельзя заблокировать свой профиль.");
  users.insert(target);}
 else if(reports){fields(body,{"conversationId","category","text"});target=text(body,"conversationId",36,36,false);require_uuid(target);
  thread=conversation(db,target,actor);users.insert(thread.at("authorId").get<std::string>());users.insert(thread.at("helperId").get<std::string>());}
 lock_users(db,users,mutation);
 if(mutation&&((requests&&action=="offers")||(offers&&action=="accept")||(chats&&action=="messages"))){
  for(const auto& participant:users){
   auto accepted=db.exec("SELECT 1 FROM users WHERE id=$1::uuid AND accepted_rules_version=$2",{participant,community_rules_version});
   if(!accepted.size())throw AppError(403,"PARTICIPANT_RULES_REQUIRED","Для продолжения собеседник должен принять действующие правила. История и завершение остаются доступны.");
  }
 }
 // Removing a pilot account also prevents new interaction with that account.
 // History, reporting, blocking and closing remain available to prior participants.
 if(config.pilot_mode&&mutation&&((requests&&action=="offers")||(offers&&action=="accept")||(chats&&action=="messages"))){
  Json ids=Json::array();for(const auto& user:users)ids.push_back(user);
  auto members=db.exec("SELECT max_user_id FROM users WHERE id IN (SELECT value::uuid FROM jsonb_array_elements_text($1::jsonb))",{ids.dump()});
  for(int i=0;i<members.size();++i)if(!pilot_allows(config,members.get(i,"max_user_id")))
   conflict("Участник больше не участвует в пилоте. Доступны история и завершение диалога.");
 }
 if(requests&&!id.empty()){
  req=request(db,id,mutation);
  if(method=="PUT"||action=="publish"||action=="cancel")own(req,"authorId",actor);
  if(method=="GET")readable_request(db,catalog,req,actor,config);
  if(method=="POST"&&action=="offers"){
   if(req.at("authorId")==actor)bad("Нельзя откликнуться на собственную заявку.");
   allow_pair(db,actor,req.at("authorId").get<std::string>());}
 }else if(offers){
  req=request(db,selected_offer.at("requestId").get<std::string>(),true);selected_offer=offer(db,id);
  allow_pair(db,req.at("authorId").get<std::string>(),selected_offer.at("helperId").get<std::string>());
 }else if(chats&&!id.empty()){
  if(mutation&&action=="close")request(db,thread.at("requestId").get<std::string>(),true);
  thread=conversation(db,id,actor,mutation);
  if(mutation)allow_pair(db,thread.at("authorId").get<std::string>(),thread.at("helperId").get<std::string>());
 }
 const auto payload_hash=sha256(body.dump()+"\n"+if_match);
 if(use_key){
  auto cached=db.exec("SELECT payload_hash,result FROM workflow_idempotency WHERE actor_id=$1::uuid AND method=$2 AND route=$3 AND key=$4::uuid",
   {actor,method,path,idempotency_key});
  if(cached.size()){
   if(cached.get(0,"payload_hash")!=payload_hash)throw AppError(409,"IDEMPOTENCY_CONFLICT","Этот ключ уже использован с другим содержимым.");
   auto result=Json::parse(cached.get(0,"result"));if(transaction)transaction->commit();return result;}
 }
 Json result;
 if(requests&&id.empty()&&method=="POST"){
  auto input=validate_request(body,catalog);auto new_id=uuid();
  db.exec("INSERT INTO help_requests(id,author_id,title,body,learning_goal,attempt,topic_id,facets,required_facets,desired_experience,taxonomy_version) "
   "VALUES($1::uuid,$2::uuid,$3,$4,$5,$6,$7,$8::jsonb,$9::jsonb,$10,$11)",
   {new_id,actor,input.at("title").get<std::string>(),input.at("body").get<std::string>(),input.at("learningGoal").get<std::string>(),
    input.at("attempt").is_null()?std::nullopt:std::optional(input.at("attempt").get<std::string>()),
    input.at("topicId").get<std::string>(),input.at("facets").dump(),input.at("requiredFacets").dump(),
    input.at("desiredExperience").is_null()?std::nullopt:std::optional(input.at("desiredExperience").get<std::string>()),
    input.at("taxonomyVersion").get<std::string>()});
  result=decorate_request(db,request(db,new_id),actor);
 }else if(requests&&id.empty()&&method=="GET"){
  const auto scope=query_value(query,"scope","mine");if(scope!="mine"&&scope!="feed")bad("scope: выберите mine или feed.");
  const auto limit=number(query_value(query,"limit","20"),1,50);
  auto [after_time,after_id]=cursor(query_value(query,"after",""));Json items=Json::array(),next=nullptr;
  if(scope=="mine"){
   auto rows=db.exec(request_columns+"WHERE r.author_id=$1::uuid AND ($2='' OR (r.created_at,r.id)<"
    "(NULLIF($2,'')::timestamptz,NULLIF($3,'')::uuid)) ORDER BY r.created_at DESC,r.id DESC LIMIT $4::int",
    {actor,after_time,after_id,std::to_string(limit+1)});
   for(int i=0;i<rows.size()&&i<limit;++i)items.push_back(decorate_request(db,request_value(rows,i),actor));
   if(rows.size()>limit)next=items.back().at("createdAt").get<std::string>()+"|"+items.back().at("id").get<std::string>();
  }else{
   HelperIndex index(db,catalog,actor,config);Json topics=Json::array();for(const auto& topic:index.request_topics)topics.push_back(topic);
   std::size_t inspected=0;bool more=!topics.empty();
   while(more&&items.size()<static_cast<std::size_t>(limit)&&inspected<1000){
    auto rows=db.exec(request_columns+
     "WHERE r.status='open' AND r.expires_at>now() AND r.author_id<>$1::uuid AND u.accepted_rules_version=$7 "
     "AND (NOT $5::boolean OR u.max_user_id IN (SELECT value FROM jsonb_array_elements_text($6::jsonb))) "
     "AND r.topic_id IN (SELECT value FROM jsonb_array_elements_text($2::jsonb)) "
     "AND NOT EXISTS(SELECT 1 FROM user_blocks b WHERE (b.blocker_id=$1::uuid AND b.blocked_id=r.author_id) "
     "OR (b.blocked_id=$1::uuid AND b.blocker_id=r.author_id)) "
     "AND ($3='' OR (r.created_at,r.id)<(NULLIF($3,'')::timestamptz,NULLIF($4,'')::uuid)) "
     "ORDER BY r.created_at DESC,r.id DESC LIMIT 100",{actor,topics.dump(),after_time,after_id,config.pilot_mode?"true":"false",Json(config.pilot_allowed_max_ids).dump(),community_rules_version});
    more=rows.size()==100;
    for(int i=0;i<rows.size();++i){
     auto value=request_value(rows,i);after_time=value.at("createdAt").get<std::string>();after_id=value.at("id").get<std::string>();++inspected;
     auto match=index.match(value);if(match){value["match"]=*match;items.push_back(decorate_request(db,std::move(value),actor));}
     if(items.size()>=static_cast<std::size_t>(limit)){more=true;break;}
    }
   }if(more&&!after_time.empty())next=after_time+"|"+after_id;
  }result={{"items",items},{"nextCursor",next}};
 }else if(requests&&method=="GET"&&action=="reach"){
  own(req,"authorId",actor);result=request_reach(db,config,catalog,req);
 }else if(requests&&method=="GET"&&action.empty()){
  result=decorate_request(db,req,actor);
 }else if(requests&&method=="PUT"){
  if(req.at("status")!="draft")conflict("Редактировать можно только черновик.");
  if(if_match.empty())throw AppError(428,"REVISION_REQUIRED","Обновите заявку перед сохранением.");
  if(if_match!="\""+std::to_string(req.at("revision").get<long long>())+"\"")
   throw AppError(409,"REQUEST_CONFLICT","Заявка уже изменена. Обновите данные.");
  auto input=validate_request(body,catalog);
  db.exec("UPDATE help_requests SET title=$2,body=$3,learning_goal=$4,attempt=$5,topic_id=$6,facets=$7::jsonb,required_facets=$8::jsonb,"
   "desired_experience=$9,taxonomy_version=$10,revision=revision+1,updated_at=now() WHERE id=$1::uuid",
   {id,input.at("title").get<std::string>(),input.at("body").get<std::string>(),input.at("learningGoal").get<std::string>(),
    input.at("attempt").is_null()?std::nullopt:std::optional(input.at("attempt").get<std::string>()),
    input.at("topicId").get<std::string>(),input.at("facets").dump(),input.at("requiredFacets").dump(),
    input.at("desiredExperience").is_null()?std::nullopt:std::optional(input.at("desiredExperience").get<std::string>()),
    input.at("taxonomyVersion").get<std::string>()});
  result=decorate_request(db,request(db,id),actor);
 }else if(requests&&action=="publish"){
  empty_body(body);if(req.at("status")!="draft")conflict("Опубликовать можно только черновик.");
  require_active_topic(catalog,req);
  Json current={{"title",req.at("title")},{"body",req.at("body")},{"learningGoal",req.at("learningGoal")},{"topicId",req.at("topicId")},
   {"facets",req.at("facets")},{"requiredFacets",req.at("requiredFacets")},{"taxonomyVersion",req.at("taxonomyVersion")}};
  if(!req.at("attempt").is_null())current["attempt"]=req.at("attempt");
  if(!req.at("desiredExperience").is_null())current["desiredExperience"]=req.at("desiredExperience");
  validate_request(current,catalog);
  auto count=db.exec("SELECT count(*) AS count FROM help_requests r JOIN topics t ON t.id=r.topic_id WHERE r.author_id=$1::uuid AND r.status='open' AND r.expires_at>now() AND t.active",{actor});
  if(std::stoi(count.get(0,"count"))>=3)conflict("Одновременно можно открыть не более трёх заявок.");
  db.exec("UPDATE help_requests SET status='open',expires_at=now()+interval '7 days',revision=revision+1,updated_at=now() WHERE id=$1::uuid",{id});
   enqueue_matching_request(db,config,id);
  result=decorate_request(db,request(db,id),actor);
 }else if(requests&&action=="cancel"){
  empty_body(body);if(req.at("status")!="draft"&&req.at("status")!="open")conflict("Эту заявку уже нельзя отменить. Завершите активный диалог.");
  db.exec("UPDATE help_requests SET status='cancelled',revision=revision+1,updated_at=now() WHERE id=$1::uuid",{id});
  db.exec("UPDATE help_offers SET status='declined',updated_at=now() WHERE request_id=$1::uuid AND status='pending'",{id});
  result=decorate_request(db,request(db,id),actor);
 }else if(requests&&action=="offers"&&method=="GET"){
  auto rows=db.exec(offer_columns+"WHERE o.request_id=$1::uuid AND ($2::boolean OR o.helper_id=$3::uuid) "
   "ORDER BY o.score DESC,o.created_at,o.id LIMIT 100",{id,req.at("authorId")==actor?"true":"false",actor});
  Json items=Json::array();for(int i=0;i<rows.size();++i)items.push_back(offer_value(rows,i));result={{"items",items}};
 }else if(requests&&action=="offers"&&method=="POST"){
  fields(body,{"message"});auto message=text(body,"message",1,1000);
  require_active_topic(catalog,req);
  if(req.at("status")!="open")conflict("Заявка больше не принимает отклики.");
  HelperIndex index(db,catalog,actor,config);auto match=index.match(req);
  if(!match)throw AppError(403,"NOT_ELIGIBLE","Проверьте компетенции, готовность помогать и свободные места.");
  if(db.exec("SELECT 1 FROM help_offers WHERE request_id=$1::uuid AND helper_id=$2::uuid",{id,actor}).size())conflict("Ваш отклик на эту заявку уже существует.");
  if(std::stoi(db.exec("SELECT count(*) AS count FROM help_offers WHERE request_id=$1::uuid",{id}).get(0,"count"))>=100)
   conflict("На эту заявку уже получено достаточно откликов.");
  auto new_id=uuid();
  db.exec("INSERT INTO help_offers(id,request_id,helper_id,message,competency_snapshot,score,narrower) "
   "VALUES($1::uuid,$2::uuid,$3::uuid,$4,$5::jsonb,$6::double precision,$7::boolean)",
   {new_id,id,actor,message,index.snapshot(*match).dump(),std::to_string(match->at("score").get<double>()),match->at("narrower").get<bool>()?"true":"false"});
  result=offer(db,new_id);
  enqueue_product_notification(db,config,"offer_received",new_id,req.at("authorId").get<std::string>(),actor,id);

 }else if(offers&&action=="withdraw"){
  empty_body(body);if(selected_offer.at("status")!="pending")conflict("Этот отклик уже нельзя отозвать.");
  db.exec("UPDATE help_offers SET status='withdrawn',updated_at=now() WHERE id=$1::uuid",{id});
  result=offer(db,id);
 }else if(offers&&action=="accept"){
  empty_body(body);require_active_topic(catalog,req);
  if(req.at("status")!="open"||selected_offer.at("status")!="pending")conflict("Этот отклик уже нельзя выбрать.");
  const auto helper=selected_offer.at("helperId").get<std::string>();
  HelperIndex index(db,catalog,helper,config);auto match=index.match(req);
  if(!match)conflict("Компетенции или доступность помощника изменились. Обновите отклики.");
  const auto request_id=req.at("id").get<std::string>(),conversation_id=uuid();
  // Eligibility is current; the offer explanation remains an immutable offer-time snapshot.
  db.exec("UPDATE help_offers SET status='accepted',updated_at=now() WHERE id=$1::uuid",{id});
  db.exec("UPDATE help_offers SET status='declined',updated_at=now() WHERE request_id=$1::uuid AND id<>$2::uuid AND status='pending'",{request_id,id});
  db.exec("INSERT INTO conversations(id,request_id,offer_id,author_id,helper_id) VALUES($1::uuid,$2::uuid,$3::uuid,$4::uuid,$5::uuid)",
   {conversation_id,request_id,id,actor,helper});
  db.exec("UPDATE help_requests SET status='in_progress',revision=revision+1,updated_at=now() WHERE id=$1::uuid",{request_id});
  result=conversation(db,conversation_id,actor);
  enqueue_product_notification(db,config,"helper_selected",conversation_id,helper,actor,request_id,conversation_id);
 }else if(chats&&id.empty()){
  auto rows=db.exec(conversation_columns+"WHERE c.author_id=$1::uuid OR c.helper_id=$1::uuid ORDER BY c.created_at DESC,c.id DESC LIMIT 100",{actor});
  Json items=Json::array();for(int i=0;i<rows.size();++i)items.push_back(conversation_value(rows,i,actor));
  result={{"items",items}};
 }else if(chats&&action=="messages"&&method=="GET"){
  auto after=number(query_value(query,"after","0"),0,9007199254740991LL);
  auto limit=number(query_value(query,"limit","50"),1,100);
  auto rows=db.exec(message_columns+"WHERE m.conversation_id=$1::uuid AND m.sequence>$2::bigint ORDER BY m.sequence LIMIT $3::int",
   {id,std::to_string(after),std::to_string(limit)});
  Json items=Json::array();for(int i=0;i<rows.size();++i)items.push_back(message_value(rows,i));
  result={{"items",items},{"nextAfter",items.empty()?after:items.back().at("sequence").get<long long>()}};
 }else if(chats&&action=="messages"&&method=="POST"){
  fields(body,{"clientMessageId","text"});const auto client_id=text(body,"clientMessageId",36,36,false),message=text(body,"text",1,2000);
  require_uuid(client_id);
  auto existing=db.exec(message_columns+"WHERE m.conversation_id=$1::uuid AND m.sender_id=$2::uuid AND m.client_message_id=$3::uuid",{id,actor,client_id});
  if(existing.size()){
   if(existing.get(0,"text")!=message)throw AppError(409,"IDEMPOTENCY_CONFLICT","Этот идентификатор сообщения уже использован с другим текстом.");
   result=message_value(existing,0);
  }else{
   if(thread.at("status")!="active")conflict("Диалог завершён: доступно только чтение.");
   auto sequence=db.exec("UPDATE conversations SET next_sequence=next_sequence+1 WHERE id=$1::uuid AND next_sequence<9007199254740991 RETURNING next_sequence",{id});
   if(!sequence.size())conflict("Достигнут предел сообщений.");
   auto message_id=uuid();
   db.exec("INSERT INTO conversation_messages(id,conversation_id,sender_id,sequence,client_message_id,text) VALUES($1::uuid,$2::uuid,$3::uuid,$4::bigint,$5::uuid,$6)",
    {message_id,id,actor,sequence.get(0,"next_sequence"),client_id,message});
   auto saved=db.exec(message_columns+"WHERE m.id=$1::uuid",{message_id});result=message_value(saved,0);
   enqueue_product_notification(db,config,"message_received",message_id,thread.at("otherUserId").get<std::string>(),actor,thread.at("requestId").get<std::string>(),id);
  }
 }else if(chats&&action=="close"){
  fields(body,{"outcome"},{"comment","nextStep"});const auto outcome=text(body,"outcome",1,30,false);
  if(outcome!="helpful"&&outcome!="partly_helpful"&&outcome!="not_helpful"&&outcome!="no_result")bad("Неизвестный результат.");
  if(thread.at("authorId")!=actor&&outcome!="no_result")throw AppError(403,"AUTHOR_OUTCOME_REQUIRED","Оценить полезность может автор вопроса.");
  if(thread.at("status")!="active")conflict("Диалог уже завершён.");
  std::optional<std::string> comment=body.contains("comment")?std::optional(text(body,"comment",0,500)):std::nullopt;
  std::optional<std::string> next_step;
  if(body.contains("nextStep")){
   const auto value=text(body,"nextStep",0,500);
   if(!value.empty()){
    if(thread.at("authorId")!=actor)throw AppError(403,"AUTHOR_OUTCOME_REQUIRED","Следующий учебный шаг может указать только автор вопроса.");
    next_step=text(body,"nextStep",1,500);
   }
  }
  db.exec("UPDATE conversations SET status='closed',closed_at=now(),outcome=$2,comment=$3,next_step=$4 WHERE id=$1::uuid",{id,outcome,comment,next_step});
  db.exec("UPDATE help_requests SET status=$2,revision=revision+1,updated_at=now() WHERE id=$1::uuid",
   {thread.at("requestId").get<std::string>(),outcome=="helpful"||outcome=="partly_helpful"?"resolved":"closed_unresolved"});
  result=conversation(db,id,actor);
 }else if(blocks){
  // Pair users are already locked in deterministic order. All pair mutations use these same locks.
  db.exec("INSERT INTO user_blocks(blocker_id,blocked_id) VALUES($1::uuid,$2::uuid) ON CONFLICT DO NOTHING",{actor,target});
  auto active=db.exec("SELECT c.id,c.request_id FROM conversations c WHERE c.status='active' AND "
   "((c.author_id=$1::uuid AND c.helper_id=$2::uuid) OR (c.author_id=$2::uuid AND c.helper_id=$1::uuid)) ORDER BY c.request_id",
   {actor,target});
  for(int i=0;i<active.size();++i){
   const auto request_id=active.get(i,"request_id");request(db,request_id,true);
   db.exec("UPDATE conversations SET status='closed',closed_at=now(),outcome='no_result' WHERE id=$1::uuid",{active.get(i,"id")});
   db.exec("UPDATE help_requests SET status='closed_unresolved',revision=revision+1,updated_at=now() WHERE id=$1::uuid",{request_id});
  }
  db.exec("UPDATE help_offers o SET status='declined',updated_at=now() FROM help_requests r WHERE o.request_id=r.id AND o.status='pending' AND "
   "((r.author_id=$1::uuid AND o.helper_id=$2::uuid) OR (r.author_id=$2::uuid AND o.helper_id=$1::uuid))",{actor,target});
  result={{"ok",true}};
 }else if(reports){
  const auto category=text(body,"category",1,20,false),description=text(body,"text",1,2000);
  if(category!="abuse"&&category!="spam"&&category!="other")bad("Неизвестная причина жалобы.");
  auto report_id=uuid();
  db.exec("INSERT INTO safety_reports(id,reporter_id,conversation_id,category,text) VALUES($1::uuid,$2::uuid,$3::uuid,$4,$5)",{report_id,actor,target,category,description});
  result={{"id",report_id},{"status","new"}};
 }else missing();
 if(use_key)db.exec("INSERT INTO workflow_idempotency(actor_id,method,route,key,payload_hash,result) VALUES($1::uuid,$2,$3,$4::uuid,$5,$6::jsonb)",
   {actor,method,path,idempotency_key,payload_hash,result.dump()});
 if(transaction)transaction->commit();
 return result;
}
} // namespace maxhelp
