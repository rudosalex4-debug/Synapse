#pragma once
#include "db.hpp"
namespace maxhelp {
// One bounded pass; no deletion of user content or idempotency records.
void maintain_workflow(Db& db);
}
