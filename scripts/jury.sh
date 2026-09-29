#!/usr/bin/env bash
# Standalone local reviewer environment; never reads .env or MAX credentials.
set -Eeuo pipefail
set +x
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd -- "$ROOT"
die() { printf 'jury: %s\n' "$*" >&2; exit 1; }
usage() {
  cat <<'HELP'
Usage: bash scripts/jury.sh up [PORT] | status | logs | stop
  up       Build the jury image, migrate the separate demo DB and start (default port 18080)
  status   Show the synapse-jury containers and current published address
  logs     Show recent backend/worker logs
  stop     Stop the jury environment; preserve the demo database
Requires Docker with Compose v2.20+ and Bash (Linux, WSL, macOS).
No domain, token, local Node/C++ installation or .env file is required.
The jury image compiles only the application; frontend build still checks types.
JURY_BUILD_JOBS controls C++ parallelism (default 2). With enough CPU/RAM:
  JURY_BUILD_JOBS=4 bash scripts/jury.sh up
HELP
}
command="${1:-up}"
if (( $# )); then shift; fi
case "$command" in
  help|--help|-h) usage; exit 0 ;;
  up)
    (( $# <= 1 )) || die 'Usage: up [PORT]'
    port="${1:-18080}"
    [[ "$port" =~ ^[1-9][0-9]{0,4}$ ]] && (( port >= 1024 && port <= 65535 )) || die 'PORT must be 1024..65535.'
    [[ "${JURY_BUILD_JOBS:-2}" =~ ^[1-9][0-9]*$ ]] || die 'JURY_BUILD_JOBS must be a positive integer.'
    ;;
  status|logs|stop) [[ $# == 0 ]] || die 'This command takes no arguments.'; port=18080 ;;
  *) usage; exit 2 ;;
esac
export JURY_PORT="$port"
command -v docker >/dev/null || die 'Install Docker with the Compose plugin first.'
docker compose version >/dev/null
docker info >/dev/null
# Explicit file, name and empty env file isolate this demo from existing deployments.
dc() (
  unset COMPOSE_PROFILES COMPOSE_FILE COMPOSE_PROJECT_NAME
  docker compose --project-directory "$ROOT" --env-file /dev/null \
    -p synapse-jury -f "$ROOT/compose.jury.yaml" "$@"
)
case "$command" in
  up)
    dc build backend
    dc up -d --wait db
    dc stop backend worker
    dc run --rm --no-deps -T init
    dc up -d --no-build --no-deps --wait backend worker
    printf '\nLocal jury demo: http://localhost:%s\nChoose Anna or Boris; accept community rules before posting.\n' "$port"
    ;;
  status) dc ps; dc port backend 8080 ;;
  logs) dc logs --tail=100 backend worker ;;
  stop) dc stop ;;
esac
