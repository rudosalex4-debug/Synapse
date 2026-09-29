#pragma once
#include "domain.hpp"
namespace maxhelp {
// Fails before serving or seeding an inconsistent editorial catalog.
void validate_catalog(const Json& catalog);
} // namespace maxhelp
