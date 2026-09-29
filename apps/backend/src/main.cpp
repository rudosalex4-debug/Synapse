#include "config.hpp"
#include "services.hpp"
#include "jobs.hpp"
#include "maintenance.hpp"
#include <iostream>
namespace maxhelp { void run_server(const Config&); }
int main(int argc,char **argv) {
 try {
  auto config=maxhelp::read_config();
  std::string command=argc==2?argv[1]:"";
  if(command=="serve")maxhelp::run_server(config);
  else if(command=="worker")maxhelp::run_worker(config);
  else if(command=="maintain"){maxhelp::Db db(config.database_url);maxhelp::maintain_workflow(db);}
  else if(command=="migrate")maxhelp::migrate(config);
  else if(command=="seed")maxhelp::seed(config,maxhelp::load_catalog(config));
  else {std::cerr<<"Usage: max-help serve|worker|migrate|seed|maintain\n";return 2;}
 }catch(const std::exception&) {maxhelp::log_event("fatal","STARTUP_OR_COMMAND_FAILED");return 1;}
 return 0;
}
