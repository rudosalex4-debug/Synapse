#include "db.hpp"
namespace maxhelp {
namespace {
std::string result_sqlstate(const PGresult* result){
 if(!result)return {};
 const char* field=PQresultErrorField(result,PG_DIAG_SQLSTATE);
 if(!field)return {};
 const std::string state=field;
 if(state.size()!=5)return {};
 for(const char c:state)if(!((c>='0'&&c<='9')||(c>='A'&&c<='Z')))return {};
 return state;
}
}
bool Result::is_null(int row,const char *column)const {
 int n=PQfnumber(value_.get(),column);
 if(n<0||row<0||row>=size())throw DbError("DB_RESULT_SHAPE");
 return PQgetisnull(value_.get(),row,n)!=0;
}
std::string Result::get(int row,const char *column)const {
 if(is_null(row,column))return {};
 return PQgetvalue(value_.get(),row,PQfnumber(value_.get(),column));
}
void Db::ensure() {
 if(connection_&&PQstatus(connection_.get())==CONNECTION_OK)return;
 if(transaction_)throw DbError("TRANSACTION_CONNECTION_LOST");
 const char *keys[]={"dbname","connect_timeout","application_name",nullptr};
 const char *values[]={url_.c_str(),"3","max-help",nullptr};
 connection_.reset(PQconnectdbParams(keys,values,1));
 if(!connection_||PQstatus(connection_.get())!=CONNECTION_OK){connection_.reset();throw DbError("DB_UNAVAILABLE");}
 std::unique_ptr<PGresult,decltype(&PQclear)> result(
 PQexec(connection_.get(),"SET statement_timeout='5s'; SET lock_timeout='2s'; SET idle_in_transaction_session_timeout='10s'"),PQclear);
 if(!result||PQresultStatus(result.get())!=PGRES_COMMAND_OK){connection_.reset();throw DbError("DB_SETUP_FAILED");}
}
Result Db::exec(const std::string &sql,const Params &params) {
 ensure(); std::vector<const char*> values; values.reserve(params.size());
 for(const auto &p:params)values.push_back(p?p->c_str():nullptr);
 PGresult *raw=PQexecParams(connection_.get(),sql.c_str(),static_cast<int>(values.size()),nullptr,values.data(),nullptr,nullptr,0);
 if(!raw)throw DbError("DB_QUERY_FAILED");
 auto status=PQresultStatus(raw);
 if(status!=PGRES_TUPLES_OK&&status!=PGRES_COMMAND_OK){
  const auto state=result_sqlstate(raw);
  PQclear(raw); if(PQstatus(connection_.get())!=CONNECTION_OK)connection_.reset();
  throw DbError("DB_QUERY_FAILED",state);
 }
 return Result(raw);
}
void Db::script(const std::string &sql) {
 ensure();std::unique_ptr<PGresult,decltype(&PQclear)> r(PQexec(connection_.get(),sql.c_str()),PQclear);
 if(!r||PQresultStatus(r.get())!=PGRES_COMMAND_OK)throw DbError("MIGRATION_FAILED",result_sqlstate(r.get()));
}
Db &thread_db(const std::string &url) {
 thread_local auto db=std::make_unique<Db>(url);
 return *db;
}
}
