#include "services.hpp"
#include "notifications.hpp"
#include "community_rules.hpp"
#include "catalog.hpp"
#include "max_bot.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <chrono>
#include <iomanip>
namespace maxhelp {
namespace {
std::string read_file(const std::filesystem::path &path) {
 std::ifstream f(path);if(!f)throw std::runtime_error("Required data file unavailable");
 std::ostringstream s;s<<f.rdbuf();auto body=s.str();body.erase(std::remove(body.begin(),body.end(),'\r'),body.end());return body;
}
std::string iso_time(int seconds) {
 auto point=std::chrono::system_clock::now()+std::chrono::seconds(seconds);
 auto time=std::chrono::system_clock::to_time_t(point);std::tm tm{};gmtime_r(&time,&tm);
 std::ostringstream s;s<<std::put_time(&tm,"%Y-%m-%dT%H:%M:%SZ");return s.str();
}
void allowed_body(const Json &body,const std::string &key) {
 if(!body.is_object()||body.size()!=1||!body.contains(key)||!body.at(key).is_string())
  throw AppError(400,"INVALID_REQUEST","Проверьте формат запроса");
}
std::string bearer(const std::string &header) {
 if(!header.starts_with("Bearer ")||header.size()<30||header.size()>256)
  throw AppError(401,"UNAUTHORIZED","Необходим повторный вход");
 auto token=header.substr(7);
 if(token.find_first_of(" \r\n\t")!=std::string::npos)throw AppError(401,"UNAUTHORIZED","Необходим повторный вход");
 return token;
}
}
Json load_catalog(const Config &config) {
 auto catalog=parse_json_strict(read_file(std::filesystem::path(config.data_dir)/"taxonomy.example.json"));
 validate_catalog(catalog);
 catalog["status"]="runtime_catalog";return catalog;
}
void migrate(const Config &config) {
 Db db(config.database_url);
 db.exec("SELECT pg_advisory_lock(7246260917)");
 db.exec("CREATE TABLE IF NOT EXISTS schema_migrations (version text PRIMARY KEY, checksum text NOT NULL, applied_at timestamptz NOT NULL DEFAULT now())");
 std::vector<std::filesystem::path> files;
 for(const auto &entry:std::filesystem::directory_iterator(config.migrations_dir))
  if(entry.is_regular_file()&&entry.path().extension()==".sql")files.push_back(entry.path());
 std::sort(files.begin(),files.end());
 if(files.empty())throw std::runtime_error("No migrations found");
 for(const auto &path:files){
  const auto body=read_file(path), name=path.filename().string(), checksum=sha256(body);
  auto old=db.exec("SELECT checksum FROM schema_migrations WHERE version=$1",{name});
  if(old.size()){
   if(old.get(0,"checksum")!=checksum){
    log_event("migration_checksum_mismatch",name);
    throw std::runtime_error("Applied migration checksum changed");
   }
   continue;
  }
  log_event("migration_started",name);
  try{
   Transaction tx(db);db.script(body);
   db.exec("INSERT INTO schema_migrations(version,checksum) VALUES($1,$2)",{name,checksum});
   tx.commit();log_event("migration_applied",name);
  }catch(const DbError& error){
   const auto state=error.sqlstate().empty()?"unavailable":error.sqlstate();
   log_event("migration_failed",name+":SQLSTATE="+state);
   throw;
  }
 }
 db.exec("SELECT pg_advisory_unlock(7246260917)");
}
void seed(const Config &config,const Json &catalog) {
 Db db(config.database_url);Transaction tx(db);db.exec("SELECT pg_advisory_xact_lock(7246260918)");
 // Preserve referenced history, but no retired/omitted topic can remain active in the DB.
 Json topic_ids=Json::array();for(const auto& topic:catalog.at("topics"))topic_ids.push_back(topic.at("id"));
 db.exec("UPDATE topics SET active=false WHERE active AND id NOT IN (SELECT value FROM jsonb_array_elements_text($1::jsonb))",{topic_ids.dump()});
 auto topics=catalog.at("topics").get<std::vector<Json>>();
 std::sort(topics.begin(),topics.end(),[](const Json&a,const Json&b){return a.at("level").get<int>()<b.at("level").get<int>();});
 for(const auto&t:topics)db.exec(
 "INSERT INTO topics(id,parent_id,level,label,aliases,active,taxonomy_version) VALUES($1,$2,$3::int,$4,$5::jsonb,$6::boolean,$7) ON CONFLICT(id) DO UPDATE SET parent_id=excluded.parent_id,level=excluded.level,label=excluded.label,aliases=excluded.aliases,active=excluded.active,taxonomy_version=excluded.taxonomy_version",
 {t.at("id").get<std::string>(),t.at("parent_id").is_null()?std::nullopt:std::optional(t.at("parent_id").get<std::string>()),
 std::to_string(t.at("level").get<int>()),t.at("label").get<std::string>(),t.value("aliases",Json::array()).dump(),t.at("active").get<bool>()?"true":"false",catalog.at("version").get<std::string>()});
 for(const auto&f:catalog.at("facets")){
  db.exec("INSERT INTO facet_definitions(id,label,applicable_topic_prefixes) VALUES($1,$2,$3::jsonb) ON CONFLICT(id) DO UPDATE SET label=excluded.label,applicable_topic_prefixes=excluded.applicable_topic_prefixes",
   {f.at("id").get<std::string>(),f.at("label").get<std::string>(),f.at("applicable_topic_prefixes").dump()});
  for(const auto&v:f.at("values"))db.exec("INSERT INTO facet_values(facet_id,id,label) VALUES($1,$2,$3) ON CONFLICT(facet_id,id) DO UPDATE SET label=excluded.label",
   {f.at("id").get<std::string>(),v.at("id").get<std::string>(),v.at("label").get<std::string>()});
 }
 if(config.demo){
  db.exec("INSERT INTO users(id,demo_persona,display_name,provenance) VALUES($1::uuid,'anna','Анна','demo'),($2::uuid,'boris','Борис','demo') ON CONFLICT(demo_persona) DO NOTHING",{uuid(),uuid()});
 }
 db.exec("INSERT INTO catalog_state(singleton,version,checksum) VALUES(true,$1,$2) ON CONFLICT(singleton) DO UPDATE SET version=excluded.version,checksum=excluded.checksum,seeded_at=now()",
  {catalog.at("version").get<std::string>(),sha256(catalog.dump())});
 tx.commit();log_event("seed_complete");
}
Json create_login(Db &db,const Config &config,const Json &body,bool demo) {
 std::string id;
 if(demo){
  if(!config.demo||config.app_env=="production")throw AppError(403,"DEMO_DISABLED","Учебный вход отключён");
  allowed_body(body,"persona");auto persona=body.at("persona").get<std::string>();
  if(persona!="anna"&&persona!="boris")throw AppError(400,"INVALID_PERSONA","Выберите учебного участника");
  auto r=db.exec("SELECT id FROM users WHERE demo_persona=$1 AND provenance='demo'",{persona});
  if(!r.size())throw AppError(503,"SEED_REQUIRED","Начальное заполнение ещё не выполнено");
  id=r.get(0,"id");
 }else{
  allowed_body(body,"initData");
  auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  auto identity=verify_init_data(body.at("initData").get<std::string>(),config.token,config.auth_max_age,now);
  if(!pilot_allows(config,identity.at("id").get<std::string>()))
   throw AppError(403,"PILOT_ACCESS_DENIED","Сейчас доступен закрытый пилот. Передайте организатору ваш MAX ID: "+identity.at("id").get<std::string>()+". После добавления в список откройте приложение снова.");
  auto r=db.exec("INSERT INTO users(id,max_user_id,display_name,provenance) VALUES($1::uuid,$2,$3,'self_declared') ON CONFLICT(max_user_id) DO UPDATE SET updated_at=now() RETURNING id",{uuid(),identity.at("id").get<std::string>(),identity.at("displayName").get<std::string>()});
  id=r.get(0,"id");
 }
 const auto token=random_token(),expires=iso_time(config.session_ttl);
 Transaction tx(db);
 db.exec("INSERT INTO sessions(token_hash,user_id,expires_at) VALUES($1,$2::uuid,$3::timestamptz)",{sha256(token),id,expires});
 db.exec("DELETE FROM sessions WHERE user_id=$1::uuid AND (expires_at<now() OR token_hash NOT IN (SELECT token_hash FROM sessions WHERE user_id=$1::uuid ORDER BY created_at DESC LIMIT 10))",{id});
 tx.commit();auto profile=get_profile(db,id);
 return Json{{"token",token},{"expiresAt",expires},{"user",{{"id",id},{"displayName",profile.at("displayName")},{"provenance",profile.at("provenance")}}}};
}
std::string authenticate(Db &db,const Config &config,const std::string &authorization) {
 auto r=db.exec("SELECT u.id,u.max_user_id FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=$1 AND s.expires_at>now() AND ($2::boolean OR u.provenance<>'demo')",{sha256(bearer(authorization)),config.demo&&config.app_env!="production"?"true":"false"});
 if(!r.size())throw AppError(401,"UNAUTHORIZED","Сессия истекла. Выполните вход заново");
 if(!pilot_allows(config,r.get(0,"max_user_id")))
  throw AppError(403,"PILOT_ACCESS_DENIED","Доступ к закрытому пилоту отключён. Обратитесь к организатору.");
 return r.get(0,"id");
}
void logout(Db &db,const std::string &authorization){db.exec("DELETE FROM sessions WHERE token_hash=$1",{sha256(bearer(authorization))});}
Json get_profile(Db &db,const std::string &id,bool own_transaction) {
 std::unique_ptr<Transaction> snapshot;if(own_transaction)snapshot=std::make_unique<Transaction>(db);
 auto u=db.exec("SELECT id,display_name,bio,available_to_help,max_active_conversations,provenance,revision FROM users WHERE id=$1::uuid FOR SHARE",{id});
 if(!u.size())throw AppError(404,"PROFILE_NOT_FOUND","Профиль не найден");
 Json profile={{"id",id},{"displayName",u.get(0,"display_name")},{"bio",u.get(0,"bio")},{"availableToHelp",u.get(0,"available_to_help")=="t"},{"maxActiveConversations",std::stoi(u.get(0,"max_active_conversations"))},{"provenance",u.get(0,"provenance")},{"revision",std::stoll(u.get(0,"revision"))},{"competencies",Json::array()},{"archivedCompetencies",Json::array()}};
 auto skills=db.exec("SELECT c.id,c.topic_id,c.facets,c.experience_kind,c.description,c.evidence_url,c.evidence_visibility,c.evidence_status,c.provenance,t.active AS topic_active FROM competencies c JOIN topics t ON t.id=c.topic_id WHERE c.user_id=$1::uuid ORDER BY c.created_at,c.id",{id});
 for(int i=0;i<skills.size();++i){
  Json c={{"id",skills.get(i,"id")},{"topicId",skills.get(i,"topic_id")},{"facets",Json::parse(skills.get(i,"facets"))},{"experienceKind",skills.get(i,"experience_kind")},{"description",skills.get(i,"description")},{"evidenceVisibility",skills.get(i,"evidence_visibility")},{"evidenceStatus",skills.get(i,"evidence_status")},{"provenance",skills.get(i,"provenance")}};
  if(!skills.is_null(i,"evidence_url"))c["evidenceUrl"]=skills.get(i,"evidence_url");
  profile[skills.get(i,"topic_active")=="t"?"competencies":"archivedCompetencies"].push_back(c);
 }
 if(snapshot)snapshot->commit();
 return profile;
}
Json save_profile(Db &db,const Json &catalog,const std::string &id,const Json &body,const std::string &if_match,bool own_transaction,const Config* config) {
 require_current_rules(db,id);
 const auto input=validate_profile(body,catalog);
 if(if_match.empty())throw AppError(428,"REVISION_REQUIRED","Обновите профиль перед сохранением");
 std::unique_ptr<Transaction> tx;if(own_transaction)tx=std::make_unique<Transaction>(db);
 auto user=db.exec("SELECT revision,provenance,available_to_help FROM users WHERE id=$1::uuid FOR UPDATE",{id});
 if(!user.size())throw AppError(404,"PROFILE_NOT_FOUND","Профиль не найден.");
 const auto before_profile=get_profile(db,id,false);
 const auto revision=user.get(0,"revision");
 if(if_match!="\""+revision+"\"")throw AppError(409,"PROFILE_CONFLICT","Профиль уже изменён. Обновите данные перед сохранением");
 // Archived competencies are read-only: forged archived IDs cannot be reused or removed.
 auto previous=db.exec("SELECT c.id FROM competencies c JOIN topics t ON t.id=c.topic_id WHERE c.user_id=$1::uuid AND t.active",{id});
 std::set<std::string> owned,keep;
 for(int i=0;i<previous.size();++i)owned.insert(previous.get(i,"id"));
 for(const auto&c:input.at("competencies")){
  auto skill=c.contains("id")?c.at("id").get<std::string>():uuid();
  if(c.contains("id")&&!owned.contains(skill))throw AppError(403,"COMPETENCY_NOT_OWNED","Компетенция недоступна для изменения");
  keep.insert(skill);
  db.exec("INSERT INTO competencies(id,user_id,topic_id,facets,experience_kind,description,evidence_url,evidence_visibility,provenance) VALUES($1::uuid,$2::uuid,$3,$4::jsonb,$5,$6,$7,$8,$9) ON CONFLICT(id) DO UPDATE SET topic_id=excluded.topic_id,facets=excluded.facets,experience_kind=excluded.experience_kind,description=excluded.description,evidence_url=excluded.evidence_url,evidence_visibility=excluded.evidence_visibility WHERE competencies.user_id=$2::uuid",
  {skill,id,c.at("topicId").get<std::string>(),c.at("facets").dump(),c.at("experienceKind").get<std::string>(),c.at("description").get<std::string>(),
  c.contains("evidenceUrl")?std::optional(c.at("evidenceUrl").get<std::string>()):std::nullopt,c.at("evidenceVisibility").get<std::string>(),user.get(0,"provenance")});
 }
 for(const auto&skill:owned)if(!keep.contains(skill))db.exec("DELETE FROM competencies WHERE id=$1::uuid AND user_id=$2::uuid",{skill,id});
 db.exec("UPDATE users SET display_name=$2,bio=$3,available_to_help=$4::boolean,max_active_conversations=$5::int,revision=revision+1,updated_at=now() WHERE id=$1::uuid",
 {id,input.at("displayName").get<std::string>(),input.at("bio").get<std::string>(),input.at("availableToHelp").get<bool>()?"true":"false",std::to_string(input.at("maxActiveConversations").get<int>())});
 auto result=get_profile(db,id,false);
 if(config&&(before_profile.at("competencies")!=result.at("competencies")||
    (user.get(0,"available_to_help")!="t"&&input.at("availableToHelp").get<bool>())))
  enqueue_matching_profile(db,*config,id);
 if(tx)tx->commit();
 return result;
}
void accept_webhook(Db &db,const Json &body){
 auto event=parse_update(body);if(!event)return;
 Transaction tx(db);
 auto inserted=db.exec("INSERT INTO bot_inbox(event_key,update_type,payload) VALUES($1,$2,$3::jsonb) ON CONFLICT(event_key) DO NOTHING RETURNING id",
 {event->key,event->type,event_json(*event).dump()});
 if(inserted.size()&&event->activity.has_value()){
  db.exec("INSERT INTO max_dialogs(max_user_id,chat_id,active,event_timestamp) VALUES($1,$2,$3::boolean,$4::bigint) ON CONFLICT(max_user_id) DO UPDATE SET chat_id=excluded.chat_id,active=excluded.active,event_timestamp=excluded.event_timestamp,updated_at=now() WHERE excluded.event_timestamp>max_dialogs.event_timestamp OR (excluded.event_timestamp=max_dialogs.event_timestamp AND excluded.active=false)",
  {event->user_id,event->chat_id,*event->activity?"true":"false",std::to_string(event->timestamp)});
  if(!*event->activity){
   db.exec("UPDATE outbox SET status='cancelled',lease_token=NULL,lock_until=NULL WHERE max_user_id=$1 AND status IN ('pending','processing') AND EXISTS(SELECT 1 FROM max_dialogs WHERE max_user_id=$1 AND active=false)",{event->user_id});
   db.exec("DELETE FROM bot_forms WHERE max_user_id=$1 AND EXISTS(SELECT 1 FROM max_dialogs WHERE max_user_id=$1 AND active=false)",{event->user_id});
  }
 }
 tx.commit();
}
}
