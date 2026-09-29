#pragma once
#include <libpq-fe.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>
namespace maxhelp {
using Params=std::vector<std::optional<std::string>>;
// Diagnostic codes only: never attach SQL, connection strings, row data or credentials.
class DbError:public std::runtime_error {
 std::string sqlstate_;
public:
 explicit DbError(const std::string &code,std::string sqlstate={}):std::runtime_error(code),sqlstate_(std::move(sqlstate)){}
 const std::string& sqlstate()const noexcept{return sqlstate_;}
};
class Result {
 std::unique_ptr<PGresult,decltype(&PQclear)> value_;
public:
 explicit Result(PGresult *p):value_(p,PQclear){}
 int size()const{return PQntuples(value_.get());}
 bool is_null(int row,const char *column)const;
 std::string get(int row,const char *column)const;
};
class Db {
 friend class Transaction; bool transaction_=false;
 std::string url_;
 std::unique_ptr<PGconn,decltype(&PQfinish)> connection_{nullptr,PQfinish};
 void ensure();
public:
 explicit Db(std::string url):url_(std::move(url)){}
 Result exec(const std::string &sql,const Params &params={});
 void script(const std::string &sql);
};
class Transaction {
 Db &db_;bool active_=true;
public:
 explicit Transaction(Db &db):db_(db){if(db_.transaction_)throw DbError("NESTED_TRANSACTION");db_.exec("BEGIN");db_.transaction_=true;}
 ~Transaction(){if(active_){try{db_.exec("ROLLBACK");}catch(...){db_.connection_.reset();}db_.transaction_=false;}}
 void commit(){db_.exec("COMMIT");db_.transaction_=false;active_=false;}
 Transaction(const Transaction&)=delete;
};
Db &thread_db(const std::string &url);
}
