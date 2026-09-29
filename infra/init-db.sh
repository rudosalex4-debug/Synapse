#!/bin/sh
set -eu
/app/bin/max-help migrate
exec /app/bin/max-help seed
