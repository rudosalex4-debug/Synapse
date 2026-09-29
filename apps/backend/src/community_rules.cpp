#include "community_rules.hpp"
namespace maxhelp {
Json community_rules(){
 return {{"version",community_rules_version},{"items",Json::array({
  Json{{"id","purpose"},{"text","Синапс — взаимопомощь в выборе образования и карьеры. Делитесь собственным опытом, честно указывайте его границы и проверяйте важные условия по актуальным источникам. Ответ участника не гарантирует поступление или трудоустройство."}},
  Json{{"id","respect"},{"text","Общайтесь уважительно. Оскорбления, давление, спам и навязчивая реклама недопустимы. Помощь добровольна: участник может отказаться или завершить разговор."}},
  Json{{"id","privacy"},{"text","Не отправляйте пароли, коды входа, платёжные данные и чужую личную информацию. Для общения достаточно переписки в Синапсе."}},
  Json{{"id","safety"},{"text","При проблеме можно пожаловаться и заблокировать собеседника. По жалобе ответственный может прочитать указанный материал и необходимый контекст; просмотр и решение записываются. Блокировка сразу прекращает новые контакты между вами."}}
 })}};
}
Json community_rules_status(Db& db,const std::string& actor){
 auto result=db.exec("SELECT accepted_rules_version=$2 AND rules_accepted_at IS NOT NULL AS accepted, "
  "CASE WHEN accepted_rules_version=$2 THEN to_char(rules_accepted_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') END AS accepted_at "
  "FROM users WHERE id=$1::uuid",{actor,community_rules_version});
 if(!result.size())throw AppError(404,"USER_NOT_FOUND","Профиль не найден.");
 return {{"version",community_rules_version},{"accepted",result.get(0,"accepted")=="t"},
  {"acceptedAt",result.is_null(0,"accepted_at")?Json(nullptr):Json(result.get(0,"accepted_at"))}};
}
Json accept_community_rules(Db& db,const std::string& actor,const Json& body){
 if(!body.is_object()||body.size()!=1||!body.contains("version")||!body.at("version").is_string())
  throw AppError(400,"INVALID_RULES_ACCEPTANCE","Передайте версию правил, которую вы прочитали.");
 if(body.at("version")!=community_rules_version)
  throw AppError(409,"RULES_VERSION_CONFLICT","Правила обновились. Прочитайте актуальную версию и подтвердите её.");
 // Repeating the same explicit acceptance preserves its original timestamp.
 auto result=db.exec("UPDATE users SET rules_accepted_at=CASE WHEN accepted_rules_version=$2 AND rules_accepted_at IS NOT NULL "
  "THEN rules_accepted_at ELSE now() END,accepted_rules_version=$2 WHERE id=$1::uuid "
  "RETURNING to_char(rules_accepted_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS accepted_at",
  {actor,community_rules_version});
 if(!result.size())throw AppError(404,"USER_NOT_FOUND","Профиль не найден.");
 return {{"version",community_rules_version},{"accepted",true},{"acceptedAt",result.get(0,"accepted_at")}};
}
void require_current_rules(Db& db,const std::string& actor){
 if(!community_rules_status(db,actor).at("accepted").get<bool>())
  throw AppError(403,"RULES_ACCEPTANCE_REQUIRED","Прочитайте и примите актуальные правила сообщества, чтобы продолжить.");
}
}
