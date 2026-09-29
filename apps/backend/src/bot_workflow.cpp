#include "bot_workflow.hpp"
#include "community_rules.hpp"
#include "notifications.hpp"
#include "workflow.hpp"
#include <algorithm>
#include <charconv>
#include <vector>
#include <map>
#include <utility>

namespace maxhelp {
namespace {
Json button(const std::string& label,const std::string& payload) {
 return {{"type","callback"},{"text",label},{"payload",payload}};
}
void row(Json& rows,const std::string& label,const std::string& payload) {
 rows.push_back(Json::array({button(label,payload)}));
}
Json rules_message(const std::string& username,bool accepted,const std::string& prefix="") {
 const auto rules=community_rules();Json rows=Json::array();
 std::string text=prefix+"Правила сообщества Синапс · "+rules.at("version").get<std::string>();
 for(const auto& item:rules.at("items"))text+="\n\n• "+item.at("text").get<std::string>();
 if(accepted)text+="\n\nВы приняли эту версию правил.";
 else{
  text+="\n\nПримите правила, чтобы задавать вопросы и помогать. История и действия безопасности доступны без нового согласия. Сохранённый ввод останется на месте.";
  row(rows,"Принимаю правила",std::string("rules:accept:")+community_rules_version);
 }
 row(rows,"Мои вопросы и отклики","requests");
 if(!username.empty())rows.push_back(Json::array({Json{{"type","open_app"},{"text","Открыть Синапс"},{"web_app",username},{"payload","profile"}}}));
 if(accepted)row(rows,"Меню","start");
 return bot_message(text,rows);
}
std::string short_text(const std::string& value,std::size_t limit) {
 std::size_t count=0,end=0;
 while(end<value.size()){
  if((static_cast<unsigned char>(value[end])&0xc0)!=0x80&&++count>limit)break;
  ++end;
 }
 return value.substr(0,end)+(end<value.size()?"…":"");
}
std::string input_text(const std::string& value,std::size_t low,std::size_t high,bool multiline=true) {
 std::size_t count=0;
 for(unsigned char c:value){
  if((c&0xc0)!=0x80)++count;
  if(c<32&&!(multiline&&(c==10||c==13||c==9)))throw AppError(400,"BOT_INPUT","В тексте есть недопустимые символы.");
 }
 if(count<low||count>high||value.find_first_not_of(" \t\r\n")==std::string::npos)
  throw AppError(400,"BOT_INPUT","Введите от "+std::to_string(low)+" до "+std::to_string(high)+" символов.");
 return value;
}
const std::map<std::string,std::string> goals={{"understand","Разобраться в теме"},{"practice","Потренироваться"},
 {"troubleshoot","Исправить ошибку"},{"learning_path","Выбрать путь обучения"}};
const std::map<std::string,std::string> experiences={{"self_study","Изучаю самостоятельно"},{"practice","Применяю на практике"},
 {"teaching","Объясняю и обучаю"},{"participation","Участвовал(а) в проектах"}};
const std::map<std::string,std::string> statuses={{"draft","Черновик"},{"open","Ищем помощника"},{"in_progress","Идёт общение"},
 {"resolved","Завершён с результатом"},{"cancelled","Отменён"},{"expired","Истёк срок"},{"closed_unresolved","Завершён без результата"}};
std::string label(const std::map<std::string,std::string>& labels,const std::string& key) {
 auto it=labels.find(key);return it==labels.end()?key:it->second;
}
const Json* topic(const Json& catalog,const std::string& id) {
 for(const auto& item:catalog.at("topics"))if(item.at("id")==id&&item.value("active",false))return &item;
 return nullptr;
}
Json profile_input(Json profile) {
 profile.erase("id");profile.erase("revision");profile.erase("provenance");profile.erase("archivedCompetencies");
 for(auto& skill:profile["competencies"]){skill.erase("provenance");skill.erase("evidenceStatus");}
 return profile;
}
constexpr std::size_t page_size=6;
std::vector<const Json*> topic_choices(const Json& catalog,const std::string& parent) {
 std::vector<const Json*> result;
 for(const auto& item:catalog.at("topics"))if(item.value("active",false)&&
  ((parent.empty()&&item.at("parent_id").is_null())||item.at("parent_id")==parent))result.push_back(&item);
 return result;
}
std::vector<const Json*> context_choices(const Json& catalog,const std::string& selected_topic) {
 std::vector<const Json*> result;
 for(const auto& facet:catalog.at("facets"))for(const auto& prefix:facet.at("applicable_topic_prefixes")){
  const auto value=prefix.get<std::string>();
  if(value=="*"||selected_topic==value||selected_topic.starts_with(value+".")){result.push_back(&facet);break;}
 }
 return result;
}
const Json* context_facet(const Json& catalog,const Json& state) {
 const auto chosen=state.value("contextFacet",std::string{});
 for(const auto* facet:context_choices(catalog,state.at("data").value("topicId",std::string{})))
  if(facet->at("id")==chosen)return facet;
 return nullptr;
}
std::size_t page_start(const Json& state,const std::string& field,std::size_t total) {
 const auto page=state.value(field,std::size_t{0});return total==0?0:std::min(page,(total-1)/page_size)*page_size;
}
std::size_t requested_page(const std::string& value,std::size_t total) {
 std::size_t page=0;const auto conversion=std::from_chars(value.data(),value.data()+value.size(),page);
 if(value.empty()||conversion.ec!=std::errc{}||conversion.ptr!=value.data()+value.size()||total==0||page>(total-1)/page_size)
  throw AppError(400,"BOT_PAGE","Выберите страницу кнопками текущего шага.");
 return page;
}
void page_buttons(Json& rows,const std::string& action,std::size_t start,std::size_t total) {
 Json controls=Json::array();
 if(start>=page_size)controls.push_back(button("← Назад по списку",action+std::to_string(start/page_size-1)));
 if(start+page_size<total)controls.push_back(button("Ещё →",action+std::to_string(start/page_size+1)));
 if(!controls.empty())rows.push_back(controls);
}
std::string context_summary(const Json& catalog,const Json& data) {
 std::string result;const auto facets=data.value("facets",Json::object());
 for(const auto* definition:context_choices(catalog,data.value("topicId",std::string{}))){
  const auto id=definition->at("id").get<std::string>();if(!facets.contains(id))continue;
  std::string values;
  for(const auto& value:definition->at("values"))
   if(std::find(facets.at(id).begin(),facets.at(id).end(),value.at("id"))!=facets.at(id).end()){
    if(!values.empty())values+=", ";values+=value.at("label").get<std::string>();
   }
  if(!values.empty())result+="\n"+definition->at("label").get<std::string>()+": "+short_text(values,180);
 }
 return result;
}
void persist(Db& db,const BotEvent& event,Json& state) {
 state["nonce"]=sha256(uuid()).substr(0,16);
 db.exec("INSERT INTO bot_forms(max_user_id,state) VALUES($1,$2::jsonb) ON CONFLICT(max_user_id) DO UPDATE SET state=excluded.state,updated_at=now()",
  {event.user_id,state.dump()});
}
Json form_message(const Json& state,const Json& catalog,const std::string& prefix="") {
 const auto step=state.at("step").get<std::string>();const auto& data=state.at("data");
 const bool question=state.at("kind")=="ask";
 const auto action="form:"+state.at("nonce").get<std::string>()+":";
 Json rows=Json::array();std::string text=prefix;
 if(step=="title")text+="Новый вопрос. Напишите короткий заголовок (1–120 символов).";
 else if(step=="body")text+="Опишите вопрос: что хотите понять и что уже пробовали (30–2000 символов).";
 else if(step=="description")text+="Что вы можете объяснить? Опишите опыт и границы знания (1–500 символов). Не добавляйте личные контакты.";
 else if(step=="topic"){
  auto parent=data.value("topicId",std::string{});auto current=topic(catalog,parent);
  if(!current)parent.clear();
  text+=current?"Уточните, о какой ситуации хотите поговорить.":"Выберите направление: образовательная траектория или карьера.";
  if(current){text+="\nСейчас: "+current->at("label").get<std::string>();
   if(current->at("level").get<int>()>=2)row(rows,"Выбрать эту тему",action+"topic_done");}
  const auto choices=topic_choices(catalog,parent);const auto start=page_start(state,"topicPage",choices.size());
  for(std::size_t i=start;i<choices.size()&&i<start+page_size;++i)
   row(rows,short_text(choices[i]->at("label").get<std::string>(),60),action+"topic:"+choices[i]->at("id").get<std::string>());
  page_buttons(rows,action+"topic_page:",start,choices.size());
  if(current)row(rows,"На уровень выше",action+"topic_up");
 }else if(step=="context"){
  const auto definition=context_facet(catalog,state);
  if(definition){
   const auto facet_id=definition->at("id").get<std::string>();
   text+="Уточнение: "+definition->at("label").get<std::string>()+". Выберите подходящие варианты (до 10); повторное нажатие снимает выбор.";
   if(definition->contains("help"))text+="\n"+definition->at("help").get<std::string>();
   const auto selected=data.value("facets",Json::object()).value(facet_id,Json::array());
   const auto& values=definition->at("values");const auto start=page_start(state,"valuePage",values.size());
   for(std::size_t i=start;i<values.size()&&i<start+page_size;++i){
    const auto& value=values.at(i);const bool checked=std::find(selected.begin(),selected.end(),value.at("id"))!=selected.end();
    row(rows,std::string(checked?"✓ ":"")+short_text(value.at("label").get<std::string>(),56),action+"facet_value:"+value.at("id").get<std::string>());
   }
   page_buttons(rows,action+"value_page:",start,values.size());
   text+="\nВыбрано: "+std::to_string(selected.size());
   row(rows,"Готово, к уточнениям",action+"facet_done");
   if(!selected.empty())row(rows,"Снять выбор в этом уточнении",action+"facet_clear");
  }else{
   text+="Добавьте контекст, если он важен: место, специальность или этап. Это необязательно; уточнения помогают найти человека с похожим опытом.";
   const auto choices=context_choices(catalog,data.at("topicId").get<std::string>());
   const auto start=page_start(state,"contextPage",choices.size());const auto selected=data.value("facets",Json::object());
   for(std::size_t i=start;i<choices.size()&&i<start+page_size;++i){
    const auto id=choices[i]->at("id").get<std::string>();
    row(rows,std::string(selected.contains(id)?"✓ ":"")+short_text(choices[i]->at("label").get<std::string>(),56),action+"facet:"+id);
   }
   page_buttons(rows,action+"context_page:",start,choices.size());
   text+=context_summary(catalog,data);
   row(rows,selected.empty()?"Продолжить без уточнений":"Продолжить с выбранным",action+"context_done");
  }
 }else if(step=="goal"){
  text+="Чего хотите достичь?";for(const auto& [key,value]:goals)row(rows,value,action+"goal:"+key);
 }else if(step=="experience"){
  text+="Какой у вас опыт в этой теме?";for(const auto& [key,value]:experiences)row(rows,value,action+"experience:"+key);
 }else if(step=="availability"){
  text+="Готовы помогать другим участникам? Эта настройка действует для всего профиля.";
  row(rows,"Да, готов(а) помогать",action+"available:yes");row(rows,"Пока только сохранить знание",action+"available:no");
 }else if(step=="confirm"){
  const auto selected=topic(catalog,data.at("topicId").get<std::string>());
  text+="Проверьте перед сохранением.\nТема: "+(selected?selected->at("label").get<std::string>():data.at("topicId").get<std::string>());
  text+=context_summary(catalog,data);
  if(question){
   text+="\nЦель: "+label(goals,data.at("learningGoal").get<std::string>())+"\n\n"+data.at("title").get<std::string>()+"\n"+data.at("body").get<std::string>();
   text+="\n\nСохраним приватный черновик. Публикация — отдельной кнопкой. Выбранный контекст улучшает подбор; обязательные условия и требования к опыту можно настроить в mini-app.";
   row(rows,"Сохранить черновик",action+"save");
  }else{
   text+="\nОпыт: "+label(experiences,data.at("experienceKind").get<std::string>())+"\n"+data.at("description").get<std::string>();
   text+=data.at("availableToHelp").get<bool>()?"\nГотовность помогать: да.":"\nГотовность помогать: нет.";
   text+="\nОстальные знания и настройки сохранятся. Подтверждение опыта можно добавить в mini-app.";
   row(rows,"Сохранить знание",action+"save");
  }
 }
 if(step!="title"&&!(step=="topic"&&!question))row(rows,"Назад",action+"back");
 row(rows,"Отменить ввод",action+"cancel");
 return bot_message(text,rows);
}
Json statistics(Db& db,const std::string& actor,const std::string& username) {
 auto counts=db.exec("SELECT CASE WHEN status='open' AND expires_at<=now() THEN 'expired' ELSE status END AS state,count(*) AS total "
  "FROM help_requests WHERE author_id=$1::uuid GROUP BY 1",{actor});
 std::string text="Ваши вопросы\n";
 if(!counts.size())text+="Пока нет вопросов.\n";
 for(int i=0;i<counts.size();++i)text+=label(statuses,counts.get(i,"state"))+": "+counts.get(i,"total")+"\n";
 auto offers=db.exec("SELECT count(*) AS total,count(DISTINCT o.helper_id) AS helpers,"
  "count(*) FILTER(WHERE o.status='accepted') AS accepted FROM help_offers o JOIN help_requests r ON r.id=o.request_id WHERE r.author_id=$1::uuid",{actor});
 text+="\nОткликов за всё время: "+offers.get(0,"total")+"\nУникальных откликнувшихся: "+offers.get(0,"helpers")+
  "\nВыбрано помощников: "+offers.get(0,"accepted")+"\nВ число откликов входят отозванные и отклонённые. Подробности — в «Мои вопросы и отклики».";
 return bot_message(text,bot_menu(username));
}
} // namespace

Outbound bot_workflow(Db& db,const Config& config,const BotEvent& event) {
 // Identity comes only from the authenticated MAX webhook, never callback payload.
 auto identity=db.exec("SELECT id FROM users WHERE max_user_id=$1",{event.user_id});
 if(!identity.size()){
  db.exec("INSERT INTO users(id,max_user_id,display_name,provenance) VALUES($1::uuid,$2,$3,'self_declared') "
   "ON CONFLICT(max_user_id) DO NOTHING",{uuid(),event.user_id,event.display_name.empty()?"Участник MAX":short_text(event.display_name,60)});
  identity=db.exec("SELECT id FROM users WHERE max_user_id=$1",{event.user_id});
 }
 const auto actor=identity.get(0,"id");
 const auto menu=bot_menu(config.bot_username);
 auto reply=[&](const std::string& text){return bot_reply(event,bot_message(text,menu));};
 const bool accepted=community_rules_status(db,actor).at("accepted").get<bool>();
 if(event.payload.starts_with("rules:accept:")){
  const auto version=event.payload.substr(13);
  if(version!=community_rules_version)return bot_reply(event,rules_message(config.bot_username,accepted,"Эта кнопка относится к прежней версии. Ниже — актуальные правила.\n\n"));
  accept_community_rules(db,actor,{{"version",version}});
  enqueue_matching_profile(db,config,actor);
  return reply("Правила сообщества приняты. Выберите действие; /resume продолжит сохранённый ввод.");
 }
 if(event.command=="rules"||(event.command=="start"&&!accepted))
  return bot_reply(event,rules_message(config.bot_username,accepted));
 if(event.command=="start"||event.command=="help")return *response_for(event,config.bot_username);
 if(event.command=="cancel"){
  db.exec("DELETE FROM bot_forms WHERE max_user_id=$1",{event.user_id});return reply("Ввод отменён. Сохранённые знания и вопросы остаются в профиле.");
 }
 const bool reading=event.command=="stats"||event.command=="requests"||event.payload.starts_with("requests:")||event.payload.starts_with("request:");
 if(!accepted&&!reading)return bot_reply(event,rules_message(config.bot_username,false));
 const auto catalog=load_catalog(config);
 auto call=[&](const std::string& method,const std::string& path,const Json& body=Json::object(),
               const std::map<std::string,std::string>& query=std::map<std::string,std::string>{}) {
  if(method!="GET")require_current_rules(db,actor);
  return workflow(db,config,catalog,actor,method,path,body,method=="POST"?uuid():"","",query,false);
 };
 if(event.command=="stats")return bot_reply(event,statistics(db,actor,config.bot_username));
 if(event.command=="requests"||event.payload.starts_with("requests:")){
  auto page=call("GET","/api/requests",Json::object(),{{"scope","mine"},{"limit","5"},
   {"after",event.payload.starts_with("requests:")?event.payload.substr(9):""}});
  Json buttons=Json::array();std::string text="Мои вопросы и отклики\n";
  for(const auto& item:page.at("items")){
   text+="\n"+item.at("title").get<std::string>()+" — "+label(statuses,item.at("status").get<std::string>());
   row(buttons,short_text(item.at("title").get<std::string>(),40),"request:"+item.at("id").get<std::string>());
  }
  if(page.at("items").empty())text+="Пока нет вопросов.";
  if(!page.at("nextCursor").is_null())row(buttons,"Следующие вопросы", "requests:"+page.at("nextCursor").get<std::string>());
  row(buttons,"К началу списка","requests");row(buttons,"Меню","start");
  return bot_reply(event,bot_message(text,buttons));
 }
 if(event.payload.starts_with("request:")||event.payload.starts_with("publish:")){
  const bool publish=event.payload.starts_with("publish:");
  const auto id=publish?event.payload.substr(8,36):event.payload.substr(8);
  // Reject foreign IDs before calling the broader workflow reader used by helpers.
  if(!db.exec("SELECT id FROM help_requests WHERE id::text=$1 AND author_id=$2::uuid",{id,actor}).size())
   throw AppError(404,"NOT_FOUND","Вопрос не найден или недоступен.");
  auto request=call("GET","/api/requests/"+id);
  if(publish){
   // Hold the same actor lock as HTTP edits through the publish transaction.
   db.exec("SELECT id FROM users WHERE id=$1::uuid FOR UPDATE",{actor});
   request=call("GET","/api/requests/"+id);
   if(event.payload!="publish:"+id+":"+request.at("revision").dump())
    throw AppError(409,"REQUEST_CONFLICT","Вопрос изменился. Откройте его заново в «Мои вопросы» и проверьте перед публикацией.");
   request=call("POST","/api/requests/"+id+"/publish");
  }
  // The API offers page is bounded to 100; aggregate the entire owned request.
  const auto counts=db.exec("SELECT count(*) AS total,count(*) FILTER(WHERE status='pending') AS pending,"
   "count(*) FILTER(WHERE status='accepted') AS accepted,count(*) FILTER(WHERE status='withdrawn') AS withdrawn,"
   "count(*) FILTER(WHERE status='declined') AS declined FROM help_offers WHERE request_id=$1::uuid",{id});
  std::string text=request.at("title").get<std::string>()+"\n"+label(statuses,request.at("status").get<std::string>())+
   "\n\n"+request.at("body").get<std::string>()+"\n\nОткликов: "+counts.get(0,"total")+
   "\nВ статусе ожидания: "+counts.get(0,"pending")+"\nПриняты: "+counts.get(0,"accepted")+
   "\nОтозваны: "+counts.get(0,"withdrawn")+"\nОтклонены: "+counts.get(0,"declined");
  text+="\nИмена, тексты откликов и выбор помощника доступны в mini-app.";
  Json buttons=Json::array();
  if(request.at("status")=="draft"&&topic(catalog,request.at("topicId").get<std::string>())&&request.at("taxonomyVersion")==catalog.at("version"))row(buttons,"Опубликовать вопрос","publish:"+id+":"+request.at("revision").dump());
  if(!topic(catalog,request.at("topicId").get<std::string>()))text+="\nТема в архиве. Сохранённая история доступна; для нового вопроса выберите направление в mini-app.";
  else if(request.at("status")=="draft"&&request.at("taxonomyVersion")!=catalog.at("version"))text+="\nКаталог изменился. Откройте черновик в mini-app, подтвердите тему и сохраните его перед публикацией.";
  row(buttons,"Обновить","request:"+id);row(buttons,"Мои вопросы","requests");
  for(const auto& item:menu)buttons.push_back(item);
  return bot_reply(event,bot_message(text,buttons));
 }
 auto rows=db.exec("SELECT state FROM bot_forms WHERE max_user_id=$1 FOR UPDATE",{event.user_id});
 Json state=rows.size()?Json::parse(rows.get(0,"state")):Json();
 // History reads do not change a saved form. Editing rechecks the current catalog.
 if(!state.is_null()&&state.value("taxonomyVersion",std::string{})!=catalog.at("version").get<std::string>()){
  state["taxonomyVersion"]=catalog.at("version");
  auto& previous=state["data"];
  if(!previous.value("topicId",std::string{}).empty()){
   previous["topicId"]="";previous["facets"]=Json::object();state["step"]="topic";
  }
  state.erase("contextFacet");state["topicPage"]=0;state["contextPage"]=0;state["valuePage"]=0;
  persist(db,event,state);
  return bot_reply(event,form_message(state,catalog,"Каталог обновлён: теперь образование и карьера. Сохранённый текст остался. Подтвердите тему и уточнения заново.\n\n"));
 }
 if(event.command=="resume")return state.is_null()?reply("Незавершённого ввода нет. Выберите действие."):bot_reply(event,form_message(state,catalog));
 if(event.command=="ask"||event.command=="knowledge"){
  if(!state.is_null())return bot_reply(event,form_message(state,catalog,"У вас уже есть незавершённый ввод. Продолжите его или отмените через /cancel.\n\n"));
  state={{"kind",event.command},{"step",event.command=="ask"?"title":"topic"},{"taxonomyVersion",catalog.at("version")},{"data",{{"facets",Json::object()}}}};
  if(event.command=="knowledge")state["profileRevision"]=get_profile(db,actor,false).at("revision");
  persist(db,event,state);return bot_reply(event,form_message(state,catalog));
 }
 if(state.is_null())return reply("Выберите действие в меню. Старые кнопки формы больше не действуют.");
 std::string action;
 if(event.payload.starts_with("form:")){
  const auto prefix="form:"+state.at("nonce").get<std::string>()+":";
  if(!event.payload.starts_with(prefix))return bot_reply(event,form_message(state,catalog,"Эта кнопка устарела. Продолжите с текущего шага.\n\n"));
  action=event.payload.substr(prefix.size());
 }else if(event.command=="back")action="back";
 else if(event.command!="text")return bot_reply(event,form_message(state,catalog));
 if(action=="cancel"){
  db.exec("DELETE FROM bot_forms WHERE max_user_id=$1",{event.user_id});return reply("Ввод отменён.");
 }
 auto& data=state["data"];const auto step=state.at("step").get<std::string>();const bool question=state.at("kind")=="ask";
 if(action=="back"){
  if(step=="body")state["step"]="title";
  else if(step=="topic"&&question)state["step"]="body";
  else if(step=="context"){
   if(context_facet(catalog,state))state.erase("contextFacet");else state["step"]="topic";
  }
  else if(step=="goal"||step=="description"){state["step"]="context";state.erase("contextFacet");}
  else if(step=="experience")state["step"]="description";
  else if(step=="availability")state["step"]="experience";
  else if(step=="confirm")state["step"]=question?"goal":"availability";
 }else if(step=="title"&&event.command=="text"){
  data["title"]=input_text(event.text,1,120,false);state["step"]="body";
 }else if(step=="body"&&event.command=="text"){
  data["body"]=input_text(event.text,30,2000);state["step"]="topic";
 }else if(step=="description"&&event.command=="text"){
  data["description"]=input_text(event.text,1,500);state["step"]="experience";
 }else if(step=="topic"){
  auto current=data.value("topicId",std::string{});
  if(!topic(catalog,current))current.clear();
  bool changed=false;
  if(action=="topic_up"){
   auto item=topic(catalog,current);data["topicId"]=item&&!item->at("parent_id").is_null()?item->at("parent_id"):Json("");changed=true;
  }else if(action.starts_with("topic_page:")){
   state["topicPage"]=requested_page(action.substr(11),topic_choices(catalog,current).size());
  }else if(action.starts_with("topic:")){
   auto item=topic(catalog,action.substr(6));
   if(!item||!((current.empty()&&item->at("parent_id").is_null())||item->at("parent_id")==current))
    throw AppError(400,"BOT_TOPIC","Выберите тему кнопкой текущего уровня.");
   data["topicId"]=item->at("id");changed=true;
  }else if(action=="topic_done"){
   auto item=topic(catalog,current);if(!item||item->at("level").get<int>()<2)throw AppError(400,"BOT_TOPIC","Выберите конкретную тему после направления.");
   state["step"]="context";state.erase("contextFacet");state["contextPage"]=0;
  }
  if(changed){
   state["topicPage"]=0;state["contextPage"]=0;state.erase("contextFacet");
   const auto allowed=context_choices(catalog,data.at("topicId").get<std::string>());
   auto old=data.value("facets",Json::object());Json retained=Json::object();
   for(const auto* facet:allowed){const auto id=facet->at("id").get<std::string>();if(old.contains(id))retained[id]=old.at(id);}
   data["facets"]=retained;
  }
 }else if(step=="context"){
  auto definition=context_facet(catalog,state);
  if(action=="facet_done")state.erase("contextFacet");
  else if(action=="context_done"&&!definition){state["step"]=question?"goal":"description";}
  else if(action.starts_with("context_page:")&&!definition){
   state["contextPage"]=requested_page(action.substr(13),context_choices(catalog,data.at("topicId").get<std::string>()).size());
  }else if(action.starts_with("facet:")&&!definition){
   const auto id=action.substr(6);bool found=false;
   for(const auto* item:context_choices(catalog,data.at("topicId").get<std::string>()))if(item->at("id")==id)found=true;
   if(!found)throw AppError(400,"BOT_CONTEXT","Выберите уточнение из текущего списка.");
   state["contextFacet"]=id;state["valuePage"]=0;
  }else if(action.starts_with("value_page:")&&definition){
   state["valuePage"]=requested_page(action.substr(11),definition->at("values").size());
  }else if(action=="facet_clear"&&definition){
   data["facets"].erase(definition->at("id").get<std::string>());
  }else if(action.starts_with("facet_value:")&&definition){
   const auto id=action.substr(12),facet_id=definition->at("id").get<std::string>();bool found=false;
   for(const auto& item:definition->at("values"))if(item.at("id")==id)found=true;
   if(!found)throw AppError(400,"BOT_CONTEXT","Выберите значение из текущего списка.");
   auto selected=data.value("facets",Json::object()).value(facet_id,Json::array());
   auto existing=std::find(selected.begin(),selected.end(),Json(id));
   if(existing!=selected.end())selected.erase(existing);
   else {if(selected.size()>=10)throw AppError(400,"BOT_CONTEXT","Выберите не более 10 значений одного уточнения.");selected.push_back(id);}
   if(!data.contains("facets"))data["facets"]=Json::object();
   if(selected.empty())data["facets"].erase(facet_id);else data["facets"][facet_id]=selected;
  }
 }else if(step=="goal"&&action.starts_with("goal:")&&goals.contains(action.substr(5))){
  data["learningGoal"]=action.substr(5);state["step"]="confirm";
 }else if(step=="experience"&&action.starts_with("experience:")&&experiences.contains(action.substr(11))){
  data["experienceKind"]=action.substr(11);state["step"]="availability";
 }else if(step=="availability"&&(action=="available:yes"||action=="available:no")){
  data["availableToHelp"]=action=="available:yes";state["step"]="confirm";
 }else if(step=="confirm"&&action=="save"){
  Json message;
  if(question){
   auto body=data;body["facets"]=data.value("facets",Json::object());body["requiredFacets"]=Json::array();body["taxonomyVersion"]=catalog.at("version");
   auto request=call("POST","/api/requests",body);Json buttons=Json::array();
   row(buttons,"Опубликовать вопрос","publish:"+request.at("id").get<std::string>()+":"+request.at("revision").dump());
   row(buttons,"Посмотреть черновик","request:"+request.at("id").get<std::string>());row(buttons,"Меню","start");
   message=bot_message("Черновик сохранён и виден только вам. Опубликовать вопрос для поиска помощника?",buttons);
  }else{
   auto profile=get_profile(db,actor,false);
   if(profile.at("revision")!=state.at("profileRevision")){
    state["profileRevision"]=profile.at("revision");persist(db,event,state);
    return bot_reply(event,form_message(state,catalog,"Профиль изменился, пока вы заполняли форму. Сохраним новое знание вместе с актуальными знаниями. Проверьте готовность помогать и подтвердите ещё раз.\n\n"));
   }
   auto input=profile_input(profile);auto skill=data;skill.erase("availableToHelp");
   skill["facets"]=data.value("facets",Json::object());skill["evidenceVisibility"]="private";
   input["competencies"].push_back(skill);input["availableToHelp"]=data.at("availableToHelp");
   require_current_rules(db,actor);
   save_profile(db,catalog,actor,input,"\""+profile.at("revision").dump()+"\"",false,&config);
   message=bot_message("Знание сохранено в вашем едином профиле. Подходящие вопросы доступны в разделе «Могу помочь» mini-app.",menu);
  }
  db.exec("DELETE FROM bot_forms WHERE max_user_id=$1",{event.user_id});return bot_reply(event,message);
 }
 persist(db,event,state);return bot_reply(event,form_message(state,catalog));
}
} // namespace maxhelp
