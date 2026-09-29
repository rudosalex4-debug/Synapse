#include "config.hpp"
#include "services.hpp"
#include "workflow.hpp"
#include "notifications.hpp"
#include "moderation.hpp"
#include "community_rules.hpp"
#include <drogon/drogon.h>
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <map>
#include <chrono>
#include <functional>
namespace maxhelp {
namespace {
using Response=drogon::HttpResponsePtr;
using Request=drogon::HttpRequestPtr;
using Callback=std::function<void(const Response&)>;
// Only fixed route labels enter diagnostics: never paths with user/resource IDs.
const char* diagnostic_route(const std::string& path){
 for(const auto* route:{"/health/live","/health/ready","/api/bootstrap","/api/taxonomy",
   "/api/auth/max","/api/auth/demo","/api/auth/logout","/api/profile",
   "/api/community-rules","/api/community-rules/status","/api/community-rules/accept",
   "/api/notification-settings","/api/blocks","/api/reports","/webhooks/max"})
  if(path==route)return route;
 for(const auto* area:{"/api/requests","/api/offers","/api/conversations","/api/moderation"})
  if(path==area||path.starts_with(std::string(area)+"/"))return area;
 return "other";
}
const char* diagnostic_method(drogon::HttpMethod method){
 if(method==drogon::Get)return "GET";
 if(method==drogon::Post)return "POST";
 if(method==drogon::Put)return "PUT";
 return "OTHER";
}
struct RequestTiming {
 using Clock=std::chrono::steady_clock;
 Clock::time_point received=Clock::now(),enqueued=received,processing=received;
 bool processing_started=false;
};
class Executor {
 std::mutex mutex_;std::condition_variable ready_;
 std::deque<std::function<void()>> tasks_;std::vector<std::thread> threads_;bool stopped_=false;
public:
 Executor(){for(int i=0;i<8;++i)threads_.emplace_back([this]{
  for(;;){std::function<void()> task;{
   std::unique_lock lock(mutex_);ready_.wait(lock,[this]{return stopped_||!tasks_.empty();});
   if(tasks_.empty()&&stopped_)return;
   task=std::move(tasks_.front());tasks_.pop_front();
  }task();}
 });}
 bool submit(std::function<void()> task){
  std::lock_guard lock(mutex_);if(stopped_||tasks_.size()>=256)return false;
  tasks_.push_back(std::move(task));ready_.notify_one();return true;
 }
 ~Executor(){{std::lock_guard lock(mutex_);stopped_=true;}ready_.notify_all();for(auto&t:threads_)t.join();}
};
class RateLimit {
 struct Bucket {std::chrono::steady_clock::time_point start;unsigned count=0;};
 std::mutex mutex_;std::map<std::string,Bucket> buckets_;
public:
 bool allow(const std::string&key,unsigned limit){
  const auto now=std::chrono::steady_clock::now();std::lock_guard lock(mutex_);
  for(auto it=buckets_.begin();it!=buckets_.end();)
   if(now-it->second.start>=std::chrono::minutes(1))it=buckets_.erase(it);else ++it;
  auto it=buckets_.find(key);
  if(it==buckets_.end()){if(buckets_.size()>=4096)return false;it=buckets_.emplace(key,Bucket{now,0}).first;}
  return ++it->second.count<=limit;
 }
};
Response json_response(const Json&body,int status,const std::string&id){
 auto result=drogon::HttpResponse::newHttpResponse();
 result->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
 result->setContentTypeCode(drogon::CT_APPLICATION_JSON);result->setBody(body.dump());
 result->addHeader("Cache-Control","no-store");result->addHeader("X-Request-ID",id);
 return result;
}
Response failure(int status,const std::string&code,const std::string&message,const std::string&id){
 auto r=json_response(Json{{"error",{{"code",code},{"message",message},{"requestId",id}}}},status,id);
 if(status==429||status==503)r->addHeader("Retry-After","2");
 return r;
}
Json body_of(const Request&r){
 if(!r->getHeader("content-type").starts_with("application/json"))
  throw AppError(415,"JSON_REQUIRED","Ожидается JSON");
 return parse_json_strict(std::string(r->body()));
}
}
void run_server(const Config&config){
 const auto catalog=load_catalog(config);
 auto executor=std::make_shared<Executor>();auto limit=std::make_shared<RateLimit>();
 auto handler=[config,catalog,executor,limit](const Request&request,Callback&&callback){
  auto timing=std::make_shared<RequestTiming>();
  const auto request_id=uuid(),path=request->path();
  // The wrapper runs for immediate rejections as well as queued responses. These
  // durations end before network delivery and do not measure client/TLS latency.
  callback=[complete=std::move(callback),timing,request_id,
    route=diagnostic_route(path),method=diagnostic_method(request->method())](const Response& response){
   const auto finished=RequestTiming::Clock::now();
   const auto milliseconds=[](auto elapsed){return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();};
   const auto total_ms=milliseconds(finished-timing->received);
   const auto queue_ms=timing->processing_started?milliseconds(timing->processing-timing->enqueued):0;
   const auto handler_ms=milliseconds(finished-(timing->processing_started?timing->processing:timing->received));
   const auto status=static_cast<int>(response->statusCode());
   if(status>=400||total_ms>=1000){
    // Drogon's logger serializes concurrent records. All fields are fixed labels,
    // generated request IDs, HTTP status codes or durations; no request data.
    LOG_WARN << Json{{"event","http_request"},{"requestId",request_id},
     {"method",method},{"route",route},{"status",status},
     {"queue_ms",queue_ms},{"handler_ms",handler_ms},{"total_ms",total_ms}}.dump();
   }
   complete(response);
  };
  if(path=="/health/live"){callback(json_response(Json{{"status","ok"}},200,request_id));return;}
  const bool auth=path.starts_with("/api/auth/");
  if(!limit->allow(request->peerAddr().toIp()+(auth?":auth":":api"),auth?30U:300U)){
   callback(failure(429,"RATE_LIMITED","Слишком много запросов. Попробуйте чуть позже",request_id));return;
  }
  if(path=="/webhooks/max"&&(config.webhook_secret.empty()||
    !constant_equals(request->getHeader("x-max-bot-api-secret"),config.webhook_secret))){
   callback(failure(403,"WEBHOOK_FORBIDDEN","Событие не подтверждено",request_id));return;
  }
  auto work=[config,catalog,request,callback,request_id,path,limit,timing]{
   timing->processing=RequestTiming::Clock::now();timing->processing_started=true;
   try{
    Json data;int status=200;
    if(path=="/health/live")data={{"status","ok"}};
    else if(path=="/api/bootstrap")data={{"mode",config.demo?"demo":"max"},{"maxConfigured",!config.token.empty()},{"version","0.12.0"},{"pilotMode",config.pilot_mode}};
    else if(path=="/api/taxonomy")data=catalog;
    else if(path=="/api/community-rules")data=community_rules();
    else {
     auto &db=thread_db(config.database_url);
     if(path=="/health/ready"){
      auto r=db.exec("SELECT (SELECT count(*) FROM schema_migrations WHERE version IN ('001_foundation.sql','002_resilience.sql','003_catalog_state.sql','004_matching_index.sql','005_workflow.sql','006_learning_outcome.sql','006_bot_forms.sql','007_product_notifications.sql','008_matching_notifications.sql','009_moderation.sql','010_community_rules.sql')) AS migrations, (SELECT count(*) FROM catalog_state WHERE singleton AND version=$1 AND checksum=$2) AS catalog",{catalog.at("version").get<std::string>(),sha256(catalog.dump())});
      if(r.get(0,"migrations")!="11"||r.get(0,"catalog")!="1")
       throw AppError(503,"NOT_READY","Инициализация базы не завершена");
      data={{"status","ready"}};
     }else if(path=="/api/auth/demo"||path=="/api/auth/max")
      data=create_login(db,config,body_of(request),path=="/api/auth/demo");
     else if(path=="/api/auth/logout"){
      logout(db,request->getHeader("authorization"));data={{"ok",true}};
     }else if(path=="/api/community-rules/status"||path=="/api/community-rules/accept"){
      const auto user=authenticate(db,config,request->getHeader("authorization"));
      if(path=="/api/community-rules/accept"){
       Transaction tx(db);data=accept_community_rules(db,user,body_of(request));
       enqueue_matching_profile(db,config,user);tx.commit();
      }else data=community_rules_status(db,user);
     }else if(path.starts_with("/api/moderation/")){
      const auto user=authenticate(db,config,request->getHeader("authorization"));
      std::map<std::string,std::string> query;
      for(const auto& [name,value]:request->getParameters())query.emplace(name,value);
      data=moderation(db,config,catalog,user,request->method()==drogon::Get?"GET":"POST",path,
       request->method()==drogon::Get?Json::object():body_of(request),request->getHeader("idempotency-key"),query);
     }else if(path=="/api/notification-settings"){
      const auto user=authenticate(db,config,request->getHeader("authorization"));
      data=request->method()==drogon::Put?save_notification_settings(db,config,user,body_of(request)):notification_settings(db,config,user);
     }else if(path=="/api/profile"){
      const auto user=authenticate(db,config,request->getHeader("authorization"));
      data=request->method()==drogon::Put?
       save_profile(db,catalog,user,body_of(request),request->getHeader("if-match"),true,&config):get_profile(db,user);
     }else if(path.starts_with("/api/requests")||path.starts_with("/api/offers")||path.starts_with("/api/conversations")||path=="/api/blocks"||path=="/api/reports"){
      const auto user=authenticate(db,config,request->getHeader("authorization"));
      if(request->method()==drogon::Get&&path.starts_with("/api/requests/")&&path.ends_with("/reach")&&!limit->allow("reach:"+user,6U))
       throw AppError(429,"RATE_LIMITED","Проверять доступность можно не чаще шести раз в минуту.");
      std::map<std::string,std::string> query;
      for(const auto &[name,value]:request->getParameters()) query.emplace(name,value);
      const auto method=request->method()==drogon::Get?"GET":request->method()==drogon::Put?"PUT":"POST";
      const auto body=request->method()==drogon::Get?Json::object():body_of(request);
      data=workflow(db,config,catalog,user,method,path,body,request->getHeader("idempotency-key"),request->getHeader("if-match"),query);
      if(request->method()==drogon::Post&&(path=="/api/requests"||path.ends_with("/offers")||path.ends_with("/messages")||path=="/api/reports"))status=201;
     }else if(path=="/webhooks/max"){accept_webhook(db,body_of(request));data={{"ok",true}};}
     else throw AppError(404,"NOT_FOUND","Адрес не найден");
    }
    auto response=json_response(data,status,request_id);
    if(path=="/api/profile")response->addHeader("ETag","\""+std::to_string(data.at("revision").get<long long>())+"\"");
    callback(response);
   }catch(const AppError&e){callback(failure(e.status,e.code,e.what(),request_id));}
   catch(const DbError&){log_event("request_failed","DB_UNAVAILABLE");callback(failure(503,"DB_UNAVAILABLE","База временно недоступна. Повторите запрос",request_id));}
   catch(const std::exception&){log_event("request_failed","INTERNAL_ERROR");callback(failure(500,"INTERNAL_ERROR","Не удалось выполнить запрос",request_id));}
  };
  // Published before submit under the executor's mutex; the worker alone then
  // updates timing and completes the response (or this thread handles rejection).
  timing->enqueued=RequestTiming::Clock::now();
  if(!executor->submit(std::move(work)))callback(failure(503,"SERVER_BUSY","Сервер занят. Повторите запрос",request_id));
 };
 auto &app=drogon::app();
 app.setLogLevel(trantor::Logger::kWarn);
 app.addListener("0.0.0.0",static_cast<uint16_t>(config.port)).setThreadNum(2)
    .setDocumentRoot(config.static_dir).setUploadPath("/tmp/max-help-uploads")
    .setClientMaxBodySize(128*1024).setClientMaxMemoryBodySize(128*1024)
    .setIdleConnectionTimeout(30).setMaxConnectionNum(1024).setMaxConnectionNumPerIP(64);
 app.registerPostHandlingAdvice([](const Request&,const Response&r){
  r->addHeader("X-Content-Type-Options","nosniff");
  r->addHeader("Referrer-Policy","no-referrer");
  r->addHeader("Content-Security-Policy","default-src 'self'; script-src 'self' https://st.max.ru; style-src 'self' 'unsafe-inline'; font-src 'self' data:; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'self'; form-action 'self'");
 });
 for(const auto &path:{"/health/live","/health/ready","/api/bootstrap","/api/taxonomy","/api/community-rules","/api/community-rules/status"})app.registerHandler(path,handler,{drogon::Get});
 for(const auto &path:{"/api/auth/demo","/api/auth/max","/api/auth/logout","/webhooks/max","/api/community-rules/accept"})app.registerHandler(path,handler,{drogon::Post});
 app.registerHandlerViaRegex("^/api/moderation/.*$",handler,{drogon::Get,drogon::Post});
 app.registerHandler("/api/profile",handler,{drogon::Get,drogon::Put});
 app.registerHandler("/api/notification-settings",handler,{drogon::Get,drogon::Put});
 app.registerHandlerViaRegex("^/api/(?:requests|offers|conversations|blocks|reports)(?:/.*)?$",handler,{drogon::Get,drogon::Post,drogon::Put});
 log_event("server_started");app.run();
}
}
