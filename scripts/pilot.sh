#!/usr/bin/env bash
# Linux / WSL entry point for the existing synapse-pilot project.
set -Eeuo pipefail
set +x
umask 077

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd -- "$ROOT"
ENV_FILE="$ROOT/.env.pilot.local"
STEP=initialization
BACKUP_FILE=
MIGRATION_STARTED=false

die() { printf 'pilot: %s\n' "$*" >&2; exit 1; }
note() { printf 'pilot: %s\n' "$*"; }
on_error() {
  local code=$?
  trap - ERR
  printf 'pilot: failed at %s (exit %s).\n' "$STEP" "$code" >&2
  if [[ -n "$BACKUP_FILE" ]]; then printf 'pilot: database backup: %s\n' "$BACKUP_FILE" >&2; fi
  if [[ "$MIGRATION_STARTED" == true ]]; then
    printf 'pilot: review the init error before restarting. No automatic database rollback was attempted.\n' >&2
  fi
  exit "$code"
}
trap on_error ERR

usage() {
  cat <<'HELP'
Usage: bash scripts/pilot.sh COMMAND

  prepare HTTPS_ORIGIN [--closed] [--allow ID,ID]
                                       Create private settings once; public MAX access by default
  prepare --prompt                     Ask for origin, bot name and public/closed mode
  access public|closed                 Change access mode in existing settings (private backup)
  doctor                               Validate Docker, settings and CA; no MAX calls
  build [--with-tests]                  Build synapse:pilot (tests skipped by default)
  deploy [--no-build|--with-tests] [--connect-max]
                                       Build, stop app, backup, migrate, start backend/worker
  connect                              Register MAX webhook and command menu explicitly
  backup                               Save a PostgreSQL snapshot under private/backups
  stop                                 Stop backend and worker; keep PostgreSQL running
  status                               Show containers and entry URLs
  logs [backend|worker|db|init] [--follow]
  max check|commands [--confirm]|subscriptions|subscribe --confirm

prepare never overwrites .env.pilot.local; access changes only PILOT_MODE.
Open an existing deployment to every authenticated MAX account:
  bash scripts/pilot.sh access public
  bash scripts/pilot.sh deploy
For later settings-only changes with an already current image:
  bash scripts/pilot.sh deploy --no-build
The script never pulls Git, deletes volumes, changes a proxy or pushes commits.
HELP
}

docker_required() {
  command -v docker >/dev/null || die 'Docker Engine and the Compose plugin are required.'
  docker compose version >/dev/null
  docker info >/dev/null
}

lock_operation() {
  command -v flock >/dev/null || die 'Install util-linux (flock) for deployment locking.'
  mkdir -p -- "$ROOT/private"
  exec 9>"$ROOT/private/pilot-operation.lock"
  flock -n 9 || die 'Another pilot operation is running in this project directory.'
}

# One environment source for BOTH the app and MAX tools. Never source an env file.
dc() (
  unset APP_PUBLIC_URL PILOT_PORT PILOT_MODE PILOT_ALLOWED_MAX_IDS POSTGRES_PASSWORD
  unset MAX_BOT_TOKEN MAX_BOT_USERNAME MAX_WEBHOOK_SECRET PILOT_MAX_CA_FILE
  unset MAX_PRODUCT_NOTIFICATIONS_ENABLED MODERATOR_MAX_IDS COMPOSE_PROFILES
  docker compose --project-directory "$ROOT" -p synapse-pilot \
    --env-file "$ENV_FILE" -f "$ROOT/compose.pilot.yaml" \
    --profile bot --profile tools "$@"
)

config_required() {
  [[ -f "$ENV_FILE" ]] || die 'Create .env.pilot.local with prepare first; see docs/DEPLOYMENT.md.'
  STEP='Compose configuration'
  dc config --quiet
}

validate_settings() {
  config_required
  STEP='pilot settings and CA'
  dc run --rm --no-deps -T --entrypoint node tools /work/scripts/pilot-config.mjs
}

max_cli() {
  STEP='MAX configuration'
  dc run --rm --no-deps -T tools "$@"
}

wait_service() {
  local service=$1 health=$2 cid state attempt
  STEP="waiting for $service"
  for ((attempt=0; attempt<60; attempt++)); do
    cid="$(dc ps --all --quiet "$service")"
    [[ -n "$cid" && "$cid" != *$'\n'* ]] || die "Expected one $service container. Run: bash scripts/pilot.sh status"
    state="$(docker inspect --format '{{.State.Status}} {{if .State.Health}}{{.State.Health.Status}}{{end}}' "$cid")"
    if [[ "$health" == yes && "$state" == 'running healthy' ]]; then return; fi
    if [[ "$health" == no && "$state" == 'running '* ]]; then return; fi
    case "$state" in exited*|dead*) die "$service stopped. Run: bash scripts/pilot.sh logs $service" ;; esac
    sleep 2
  done
  die "$service did not become ready within 120 seconds. Run: bash scripts/pilot.sh logs $service"
}

build_image() {
  STEP='image build'
  note "Building application (RUN_TESTS=$1)."
  dc build --build-arg "RUN_TESTS=$1" backend
}

backup_db() {
  STEP='database backup'
  mkdir -p -- "$ROOT/private/backups"
  local partial
  partial="$(mktemp "$ROOT/private/backups/pilot-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX.dump.partial")"
  if ! dc exec -T db pg_dump -U pilot -d synapse_pilot -Fc >"$partial"; then
    die "Backup failed; migration was not started. Incomplete file: $partial"
  fi
  [[ -s "$partial" ]] || die "Backup is empty; migration was not started. File: $partial"
  BACKUP_FILE="${partial%.partial}"
  mv -- "$partial" "$BACKUP_FILE"
  note "Database backup: $BACKUP_FILE"
}

entry_urls() {
  dc exec -T backend sh -c '
    printf "Mini-app: %s\n" "$APP_PUBLIC_URL"
    printf "Open in MAX: https://max.ru/%s\n" "$MAX_BOT_USERNAME"
  '
}

require_live_settings() {
  STEP='matching live MAX settings'
  local service cid
  for service in backend worker; do
    cid="$(dc ps --status running --quiet "$service")"
    if [[ -z "$cid" ]]; then
      [[ "$service" == worker ]] && continue
      die 'Backend is not running. Deploy before configuring MAX.'
    fi
    [[ "$cid" != *$'\n'* ]] || die "Expected one $service container."
    docker inspect --format '{{json .Config.Env}}' "$cid" |
      dc run --rm --no-deps -T --entrypoint node tools /work/scripts/pilot-config.mjs --compare-live
  done
}

connect_max() {
  wait_service backend yes
  require_live_settings
  # These explicit setup actions operate on the same credentials as the containers.
  max_cli check
  max_cli subscribe --confirm
  max_cli commands --confirm
  max_cli subscriptions
  note 'MAX configuration applied. Open the bot and send a new /start.'
}

command=${1:-help}
if (($#)); then shift; fi
case "$command" in help|--help|-h) usage; exit 0 ;; esac
docker_required

case "$command" in
  prepare)
    lock_operation
    STEP='prepare environment'
    [[ ! -e "$ENV_FILE" ]] || die '.env.pilot.local already exists; its secrets were preserved. Use doctor or deploy.'
    [[ -f "$ROOT/private/max-ca.pem" ]] || die 'Place the official MAX CA bundle in private/max-ca.pem first; see docs/DEPLOYMENT.md.'
    [[ -f "$ROOT/.env.max.local" ]] || die 'Create .env.max.local with MAX_BOT_TOKEN and MAX_BOT_USERNAME first. Never pass the token as a shell argument.'
    tty_flags=()
    if [[ "${1:-}" == --prompt ]]; then
      [[ $# == 1 && -t 0 && -t 1 ]] || die 'prepare --prompt needs an interactive terminal.'
      tty_flags=(-it)
    fi
    docker run --rm "${tty_flags[@]}" --user "$(id -u):$(id -g)" \
      --mount "type=bind,src=$ROOT,dst=/work" -w /work \
      node:24-bookworm-slim node scripts/prepare-pilot.mjs "$@"
    ;;
  access)
    [[ $# == 1 && ( "$1" == public || "$1" == closed ) ]] || die 'Usage: access public|closed'
    setting_args=("$1")
    lock_operation
    STEP='updating release settings'
    [[ -f "$ENV_FILE" ]] || die 'Create .env.pilot.local with prepare first.'
    docker run --rm --user "$(id -u):$(id -g)" \
      --mount "type=bind,src=$ROOT,dst=/work" -w /work \
      node:24-bookworm-slim node scripts/pilot-access.mjs "${setting_args[@]}"
    note 'Release settings saved. Apply with: bash scripts/pilot.sh deploy'
    ;;
  doctor)
    [[ $# == 0 ]] || die 'doctor takes no arguments.'
    validate_settings
    note 'Local deployment configuration is valid. MAX and public HTTPS were not contacted.'
    ;;
  build)
    tests=false
    if [[ $# == 1 && "$1" == --with-tests ]]; then tests=true
    elif [[ $# != 0 ]]; then die 'Usage: build [--with-tests]'; fi
    lock_operation
    validate_settings
    build_image "$tests"
    ;;
  deploy)
    build=true; tests=false; connect=false
    for option in "$@"; do
      case "$option" in
        --no-build) build=false ;;
        --with-tests) tests=true ;;
        --connect-max) connect=true ;;
        *) die 'Usage: deploy [--no-build|--with-tests] [--connect-max]' ;;
      esac
    done
    [[ "$build" == true || "$tests" == false ]] || die '--with-tests cannot be combined with --no-build.'
    lock_operation
    validate_settings
    if [[ "$build" == true ]]; then build_image "$tests"
    else docker image inspect synapse:pilot >/dev/null; fi
    STEP='PostgreSQL startup'
    # Do not recreate an existing DB when only app settings changed.
    dc up -d --no-recreate db
    wait_service db yes
    STEP='stopping app writers'
    dc stop backend worker
    backup_db
    STEP='database migrations and catalog'
    MIGRATION_STARTED=true
    dc run --rm --no-deps -T init
    STEP='backend startup'
    dc up -d --no-build --no-deps --force-recreate backend
    wait_service backend yes
    STEP='worker startup'
    dc up -d --no-build --no-deps --force-recreate worker
    wait_service worker no
    MIGRATION_STARTED=false
    if [[ "$connect" == true ]]; then connect_max; fi
    dc ps
    entry_urls
    note 'Deployment completed. For first MAX connection: bash scripts/pilot.sh connect'
    ;;
  connect)
    [[ $# == 0 ]] || die 'connect takes no arguments.'
    lock_operation
    validate_settings
    connect_max
    ;;
  backup)
    [[ $# == 0 ]] || die 'backup takes no arguments.'
    lock_operation
    config_required
    wait_service db yes
    backup_db
    ;;
  stop)
    [[ $# == 0 ]] || die 'stop takes no arguments.'
    lock_operation
    config_required
    STEP='stopping application'
    dc stop backend worker
    note 'Backend and worker stopped. Database and volumes preserved.'
    ;;
  status)
    [[ $# == 0 ]] || die 'status takes no arguments.'
    config_required
    dc ps -a
    if [[ -n "$(dc ps --status running --quiet backend)" ]]; then entry_urls; fi
    ;;
  logs)
    config_required
    services=(); follow=()
    for option in "$@"; do
      case "$option" in
        backend|worker|db|init) services+=("$option") ;;
        --follow) follow=(--follow) ;;
        *) die 'Usage: logs [backend|worker|db|init] [--follow]' ;;
      esac
    done
    if ((${#services[@]} == 0)); then services=(backend worker); fi
    dc logs --tail=80 "${follow[@]}" "${services[@]}"
    ;;
  max)
    [[ $# -gt 0 ]] || die 'Usage: max check|commands [--confirm]|subscriptions|subscribe --confirm'
    case "$1" in check|commands|subscriptions|subscribe) ;; *) die 'Unknown MAX operation.' ;; esac
    lock_operation
    config_required
    if [[ "$1" == subscribe || ( "$1" == commands && "${2:-}" == --confirm ) ]]; then
      require_live_settings
    fi
    max_cli "$@"
    ;;
  *) usage; die "Unknown command: $command" ;;
esac
