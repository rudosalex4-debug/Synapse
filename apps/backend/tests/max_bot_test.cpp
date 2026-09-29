#include "max_bot.hpp"
#include <iostream>
#include <stdexcept>
using namespace maxhelp;
namespace {
int count=0;
void check(bool value,const char*name){if(!value)throw std::runtime_error(name);++count;}
template<typename F>void api_error(F f,int status,bool retry){try{f();throw std::runtime_error("expected API failure");}catch(const MaxApiError&e){check(e.status==status,"status");check(e.retryable==retry,"retry");}}
Json started(){return {{"update_type","bot_started"},{"timestamp",1000},{"chat_id",42},{"user",{{"user_id",123},{"is_bot",false}}}};}
}
int main(){try{
 auto update=started();auto event=parse_update(update);check(event.has_value(),"start parsed");check(event->activity==true,"active");
 update["user"]["first_name"]="changed";check(parse_update(update)->key==event->key,"identity excludes profile");
 auto roundtrip=event_from_json(event_json(*event));check(roundtrip.key==event->key,"roundtrip");
 auto response=response_for(*event,"test_bot");check(response.has_value(),"start response");
 check(response->body["attachments"][0]["payload"]["buttons"][0][0]["type"]=="open_app","open app button");
 check(response->body["attachments"][0]["payload"]["buttons"][0][0]["web_app"]=="test_bot","username");
 check(response_for(*event,"")->body["attachments"][0]["payload"]["buttons"].size()==6,"menu without unconfigured app button");
 update["update_type"]="bot_stopped";check(!response_for(*parse_update(update),"test"),"stop no response");
 update["update_type"]="dialog_removed";check(parse_update(update)->activity==false,"removed inactive");
 update["update_type"]="unknown";check(!parse_update(update),"unknown ignored");
 update=started();update["user"]["is_bot"]=true;check(!parse_update(update),"ignore bots");
 Json message={{"update_type","message_created"},{"timestamp",2000},{"message",{{"recipient",{{"chat_type","dialog"},{"chat_id",42}}},{"sender",{{"user_id",123},{"is_bot",false}}},{"body",{{"mid","mid-1"},{"text","/help private words"}}}}}};
 auto help=parse_update(message);check(help->command=="help","help");check(event_json(*help).dump().find("private words")==std::string::npos,"raw text discarded");
 check(!help->activity.has_value(),"message cannot reactivate");
 message["message"]["body"]["text"]="/start";
 check(parse_update(message)->activity==true,"explicit start establishes existing chat");
 message["message"]["recipient"]["chat_type"]="chat";check(!parse_update(message),"group ignored");
 message["message"]["recipient"]["chat_type"]="dialog";message["message"]["body"]["text"]="/starter";check(response_for(*parse_update(message),"").has_value(),"unknown command returns menu");
 for(const auto* command:{"ask","knowledge","stats","requests","cancel","back","resume"}){
  message["message"]["body"]["text"]="/"+std::string(command);
  check(parse_update(message)->command==command,"product command parsed");
 }
 message["message"]["body"]["text"]="Помогите понять решение задачи";
 auto input=*parse_update(message);check(input.command=="text","form text parsed");
 check(event_from_json(event_json(input)).text==input.text,"form text survives restart");
 message["update_type"]="message_callback";message["callback"]={{"callback_id","cb/id"},{"payload","help"},{"user",{{"user_id",123},{"is_bot",false}}}};
 auto answer=response_for(*parse_update(message),"bot");check(answer->kind=="answer","callback answer");
 message["callback"]["payload"]="form:0123456789abcdef:topic:science.math";
 auto form=*parse_update(message);check(event_from_json(event_json(form)).payload==form.payload,"form callback survives restart");
 message["message"]=nullptr;check(!parse_update(message),"callback missing context ignored");
 int calls=0;
 MaxClient client("synthetic-token","https://example.invalid",[&](const HttpRequest&r){++calls;check(r.authorization=="synthetic-token","raw token");check(r.url=="https://example.invalid/answers?callback_id=cb%2Fid","encoded callback");return HttpResult{200,R"({"success":true})",0};});
 client.send(*answer);check(calls==1,"one attempt per call");
 api_error([]{check_max_response({429,"",9});},429,true);
 api_error([]{check_max_response({503,"",0});},503,true);
 api_error([]{check_max_response({401,"",0});},401,false);
 api_error([]{check_max_response({200,R"({"success":false})",0});},200,false);
 api_error([]{check_max_response({200,"not json",0});},200,false);
 api_error([]{check_max_response({200,"{}",0});},200,false);
 check_max_response({200,R"({"message":{"body":{"mid":"1"}}})",0});++count;
 api_error([&]{MaxClient("","https://example.invalid").send(*response);},0,false);
 try{MaxClient bad("token\r\nInjected: x");throw std::runtime_error("header injection accepted");}catch(const AppError&){++count;}
 try{MaxClient bad("token","http://example.invalid");throw std::runtime_error("insecure URL accepted");}catch(const AppError&){++count;}
 std::cout<<"Passed "<<count<<" MAX adapter checks\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
}
