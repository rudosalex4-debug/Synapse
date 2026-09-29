#!/usr/bin/env bash
# Run in the WSL distribution that owns the project's Docker Engine:
#   SYNAPSE_TEST_IMAGE=synapse:local bash scripts/migration-upgrade-test.sh
# Only disposable, uniquely named resources are created. No .env, published
# ports, project volumes, real credentials, worker, or MAX transport is used.
set -Eeuo pipefail

image="${SYNAPSE_TEST_IMAGE:-synapse:local}"
postgres_image="postgres:17-bookworm"
run_id="synapse-upgrade-$(date +%s)-$$-$RANDOM"
network="${run_id}-net"
db_container="${run_id}-db"
app_container="${run_id}-app"
resource_label="org.synapse.migration-test"
tmp_base="${TMPDIR:-/tmp}"
tmp_base="$(cd -- "$tmp_base" && pwd -P)"
scratch="$(mktemp -d "$tmp_base/synapse-migration-upgrade.XXXXXXXX")"
test_user="synapse_upgrade_test"
test_password="synthetic-upgrade-password"

cleanup() {
  local result=$? current_label
  trap - EXIT INT TERM
  for container in "$app_container" "$db_container"; do
    current_label="$(docker container inspect --format '{{ index .Config.Labels "org.synapse.migration-test" }}' "$container" 2>/dev/null || true)"
    if [[ "$current_label" == "$run_id" ]]; then
      if ! docker container rm -f "$container" >/dev/null 2>&1; then
        printf 'Could not remove test container %s\n' "$container" >&2
        result=1
      fi
    fi
  done
  current_label="$(docker network inspect --format '{{ index .Labels "org.synapse.migration-test" }}' "$network" 2>/dev/null || true)"
  if [[ "$current_label" == "$run_id" ]]; then
    if ! docker network rm "$network" >/dev/null 2>&1; then
      printf 'Could not remove test network %s\n' "$network" >&2
      result=1
    fi
  fi
  # The directory comes directly from mktemp. Refuse any broader cleanup target.
  if [[ "$scratch" == "$tmp_base"/synapse-migration-upgrade.* && -d "$scratch" && ! -L "$scratch" ]]; then
    rm -rf -- "$scratch" || result=1
  fi
  exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

command -v docker >/dev/null
command -v tar >/dev/null
# Require existing images; this gate never pulls or rebuilds anything itself.
docker image inspect "$image" >/dev/null
docker image inspect "$postgres_image" >/dev/null
mkdir -p "$scratch/full" "$scratch/baseline_a" "$scratch/baseline_b"
# Read the exact migration bytes shipped in the image being tested.
docker run --rm --name "$app_container" --label "$resource_label=$run_id" \
  --network none --entrypoint tar "$image" -C /app/db/migrations -cf - . \
  | tar -xf - -C "$scratch/full"
common=(001_foundation.sql 002_resilience.sql 003_catalog_state.sql 004_matching_index.sql 005_workflow.sql)
for filename in "${common[@]}"; do
  cp -- "$scratch/full/$filename" "$scratch/baseline_a/$filename"
  cp -- "$scratch/full/$filename" "$scratch/baseline_b/$filename"
done
cp -- "$scratch/full/006_learning_outcome.sql" "$scratch/baseline_a/006_learning_outcome.sql"
cp -- "$scratch/full/006_bot_forms.sql" "$scratch/baseline_b/006_bot_forms.sql"
# The application image runs as its non-root user.
chmod 755 "$scratch/baseline_a" "$scratch/baseline_b"
chmod 644 "$scratch/baseline_a/"*.sql "$scratch/baseline_b/"*.sql

docker network create --internal --label "$resource_label=$run_id" "$network" >/dev/null
# tmpfs overrides postgres:17's data volume. No persistent volume is created.
docker run -d --rm --name "$db_container" --label "$resource_label=$run_id" \
  --network "$network" --network-alias upgrade-db \
  --tmpfs /var/lib/postgresql/data:rw,nosuid,size=256m \
  --env "POSTGRES_USER=$test_user" --env "POSTGRES_PASSWORD=$test_password" \
  --env POSTGRES_DB=postgres "$postgres_image" >/dev/null
ready=false
for ((attempt=0; attempt<60; ++attempt)); do
  if docker exec "$db_container" pg_isready -h 127.0.0.1 -U "$test_user" -d postgres >/dev/null 2>&1; then
    ready=true
    break
  fi
  sleep 0.5
done
if [[ "$ready" != true ]]; then
  printf 'Disposable PostgreSQL did not become ready.\n' >&2
  exit 1
fi

sql() {
  local database=$1
  shift
  docker exec -i --env "PGPASSWORD=$test_password" "$db_container" \
    psql -X -U "$test_user" -d "$database" --set ON_ERROR_STOP=1 "$@"
}
run_app() {
  local database=$1 action=$2 subset=${3:-}
  local -a options=(run --rm --name "$app_container" --label "$resource_label=$run_id"
    --network "$network"
    --env "DATABASE_URL=postgresql://$test_user:$test_password@upgrade-db:5432/$database"
    --env APP_ENV=test --env DEMO_MODE=false --env PILOT_MODE=false
    --env MAX_DELIVERY_ENABLED=false --env MAX_PRODUCT_NOTIFICATIONS_ENABLED=false
    --env MAX_BOT_TOKEN= --env MAX_WEBHOOK_SECRET= --env MAX_BOT_USERNAME=
    --env DATA_DIR=/app/data --env MIGRATIONS_DIR=/app/db/migrations)
  if [[ -n "$subset" ]]; then
    options+=(--mount "type=bind,source=$scratch/$subset,target=/test-migrations,readonly"
      --env MIGRATIONS_DIR=/test-migrations)
  fi
  if [[ "$action" == queue-tests || "$action" == bot-workflow-tests ]]; then
    docker "${options[@]}" "$image" "/app/bin/$action"
  else
    docker "${options[@]}" "$image" /app/bin/max-help "$action"
  fi
}

for database in baseline_a baseline_b; do
  printf 'Preparing %s baseline...\n' "$database"
  sql postgres -c "CREATE DATABASE $database" >/dev/null
  run_app "$database" migrate "$database"
  run_app "$database" seed "$database"
  sql "$database" <<'SQL'
DO $$ BEGIN
  IF (SELECT count(*) FROM schema_migrations) <> 6 THEN
    RAISE EXCEPTION 'Expected six historical migrations';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM catalog_state WHERE singleton) THEN
    RAISE EXCEPTION 'Baseline catalog seed missing';
  END IF;
END $$;
CREATE TABLE migration_upgrade_original_ledger AS
  SELECT version,checksum,applied_at FROM schema_migrations;
INSERT INTO users(id,max_user_id,display_name,bio,available_to_help,max_active_conversations,provenance,revision)
VALUES('11111111-1111-4111-8111-111111111111','900000000001','Upgrade marker',
       'Preserve this profile across upgrades',false,3,'self_declared',42);
SQL
  if [[ "$database" == baseline_b ]]; then
    sql "$database" <<'SQL'
INSERT INTO max_dialogs(max_user_id,chat_id,active,event_timestamp)
VALUES('900000000001','-900000000001',true,100);
INSERT INTO bot_forms(max_user_id,state)
VALUES('900000000001',
       '{"kind":"ask","step":"body","nonce":"0123456789abcdef","data":{"title":"Unfinished upgrade question"}}'::jsonb);
SQL
  fi

  # The second full pass must be a no-op with the same historical checksums.
  run_app "$database" migrate
  run_app "$database" migrate
  sql "$database" <<'SQL'
DO $$ BEGIN
  IF (SELECT array_agg(version ORDER BY version) FROM schema_migrations)
      IS DISTINCT FROM ARRAY['001_foundation.sql','002_resilience.sql','003_catalog_state.sql',
        '004_matching_index.sql','005_workflow.sql','006_bot_forms.sql',
        '006_learning_outcome.sql','007_product_notifications.sql','008_matching_notifications.sql',
        '009_moderation.sql','010_community_rules.sql']::text[] THEN
    RAISE EXCEPTION 'Expected exactly the eleven integrated migrations';
  END IF;
  IF EXISTS (
    SELECT 1 FROM migration_upgrade_original_ledger old
    LEFT JOIN schema_migrations current USING(version)
    WHERE current.version IS NULL OR current.checksum IS DISTINCT FROM old.checksum
      OR current.applied_at IS DISTINCT FROM old.applied_at
  ) THEN
    RAISE EXCEPTION 'An existing ledger entry was changed or reapplied';
  END IF;
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_schema='public' AND table_name='conversations' AND column_name='next_step'
  ) THEN
    RAISE EXCEPTION 'Learning outcome column missing after upgrade';
  END IF;
  IF to_regclass('public.bot_forms') IS NULL THEN
    RAISE EXCEPTION 'Bot form table missing after upgrade';
  END IF;
  IF to_regclass('public.matching_notification_jobs') IS NULL
      OR to_regclass('public.matching_notification_dispatches') IS NULL
      OR to_regclass('public.matching_notification_refreshes') IS NULL THEN
    RAISE EXCEPTION 'Matching notification tables missing after upgrade';
  END IF;
  IF to_regclass('public.moderation_audit') IS NULL THEN
    RAISE EXCEPTION 'Moderation audit table missing after upgrade';
  END IF;
  IF EXISTS (
    SELECT 1 FROM (VALUES
      ('users','product_notifications_since'),('users','accepted_rules_version'),('users','rules_accepted_at'),
      ('safety_reports','queue_id'),('safety_reports','revision'),('safety_reports','resolution'),
      ('safety_reports','resolution_note'),('safety_reports','resolved_by'),('safety_reports','resolved_at'),
      ('conversations','moderation_closed')
    ) AS expected(table_name,column_name)
    WHERE NOT EXISTS (
      SELECT 1 FROM information_schema.columns actual
      WHERE actual.table_schema='public' AND actual.table_name=expected.table_name
        AND actual.column_name=expected.column_name
    )
  ) THEN
    RAISE EXCEPTION 'Notification, moderation or community rules columns missing after upgrade';
  END IF;
  IF NOT EXISTS (
    SELECT 1 FROM users WHERE id='11111111-1111-4111-8111-111111111111'::uuid
      AND max_user_id='900000000001' AND display_name='Upgrade marker'
      AND bio='Preserve this profile across upgrades' AND NOT available_to_help
      AND max_active_conversations=3 AND provenance='self_declared' AND revision=42
      AND NOT product_notifications AND product_notifications_since IS NULL
      AND accepted_rules_version IS NULL AND rules_accepted_at IS NULL
  ) THEN
    RAISE EXCEPTION 'Profile marker changed, notifications became enabled or rules were accepted implicitly';
  END IF;
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_schema='public' AND table_name='outbox' AND column_name='product_context'
  ) THEN
    RAISE EXCEPTION 'Product notification metadata column missing';
  END IF;
END $$;
SQL
  if [[ "$database" == baseline_b ]]; then
    sql "$database" <<'SQL'
DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM bot_forms WHERE max_user_id='900000000001'
      AND state='{"kind":"ask","step":"body","nonce":"0123456789abcdef","data":{"title":"Unfinished upgrade question"}}'::jsonb
  ) OR (SELECT count(*) FROM bot_forms) <> 1 THEN
    RAISE EXCEPTION 'Unfinished bot form was lost or changed during upgrade';
  END IF;
  IF NOT EXISTS (
    SELECT 1 FROM max_dialogs WHERE max_user_id='900000000001'
      AND chat_id='-900000000001' AND active AND event_timestamp=100
  ) THEN
    RAISE EXCEPTION 'Bot dialog marker changed during upgrade';
  END IF;
END $$;
SQL
  else
    sql "$database" <<'SQL'
DO $$ BEGIN
  IF EXISTS (SELECT 1 FROM bot_forms) THEN
    RAISE EXCEPTION 'Upgrade created an unexpected bot form';
  END IF;
END $$;
SQL
  fi
  if [[ "$database" == baseline_a ]]; then
    run_app "$database" queue-tests
    run_app "$database" bot-workflow-tests
  fi
  printf 'PASS %s: both full migration passes, original ledger, profile and bot-form preservation.\n' "$database"
done
printf 'PASS migration upgrade gate: both historical branches upgrade to the integrated schema.\n'
