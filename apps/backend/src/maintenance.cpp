#include "maintenance.hpp"
namespace maxhelp {
void maintain_workflow(Db& db) {
 auto due=db.exec("SELECT id,author_id FROM help_requests WHERE status='open' AND expires_at<=now() ORDER BY expires_at,id LIMIT 100");
 for(int i=0;i<due.size();++i){
  Transaction tx(db);
  // Request mutations always lock users first. Only the author is needed here:
  // accept, cancel, offer and block also require this author lock.
  db.exec("SELECT id FROM users WHERE id=$1::uuid FOR UPDATE",{due.get(i,"author_id")});
  auto current=db.exec("SELECT id FROM help_requests WHERE id=$1::uuid AND status='open' AND expires_at<=now() FOR UPDATE",{due.get(i,"id")});
  if(current.size()){
   db.exec("UPDATE help_requests SET status='expired',revision=revision+1,updated_at=now() WHERE id=$1::uuid",{due.get(i,"id")});
   db.exec("UPDATE help_offers SET status='declined',updated_at=now() WHERE request_id=$1::uuid AND status='pending'",{due.get(i,"id")});
  }
  tx.commit();
 }
}
}
