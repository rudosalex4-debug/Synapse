// Build synapse:local first, then: node scripts/auth-integration-test.mjs
// Owns only compose.auth-test.yaml / synapse-auth-test. The normal demo is untouched.
// The isolated PostgreSQL volume is retained; no Docker volume is deleted here.
import assert from 'node:assert/strict';
import { scenarios } from './check-catalog-scenarios.mjs';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { execFile } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execute = promisify(execFile);
const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const projectName = 'synapse-auth-test';
const composeFile = 'compose.auth-test.yaml';
const testBotToken = 'local-test-token-not-a-real-bot-token';
const testImage = process.env.SYNAPSE_TEST_IMAGE || 'synapse:local';
const api = new URL('http://127.0.0.1:8082');
const demoApi = new URL('http://127.0.0.1:8083');
const allowedOrigins = new Set([api.origin, demoApi.origin]);
const maxId = '9223372036854775807';
const adjacentId = '9223372036854775806';
let checks = 0;
const sessions = [];

function check(condition, message) {
  // Avoid printing bearer tokens or signed payloads even when an assertion fails.
  assert.ok(condition, message);
  checks++;
}

async function compose(args, { allowFailure = false, timeout = 45000 } = {}) {
  const common = ['compose', '--project-name', projectName, '-f', composeFile];
  let executable = 'docker';
  let commandArgs = common;
  if (process.platform === 'win32') {
    const match = /^([A-Za-z]):\/(.+)$/.exec(projectRoot.replaceAll('\\', '/'));
    if (!match) throw new Error('Run this test inside WSL for a UNC workspace.');
    const linuxRoot = '/mnt/' + match[1].toLowerCase() + '/' + match[2];
    executable = 'wsl';
    commandArgs = ['-d', process.env.SYNAPSE_WSL_DISTRO || 'kali-linux', '--', 'env', 'COMPOSE_DISABLE_ENV_FILE=1', 'SYNAPSE_TEST_IMAGE=' + (process.env.SYNAPSE_TEST_IMAGE || 'synapse:local'), 'docker', ...common,
      '--project-directory', linuxRoot, '-f', linuxRoot + '/' + composeFile];
    // The absolute filename is necessary when Node starts outside the project.
    commandArgs.splice(commandArgs.indexOf('-f'), 2);
  } else {
    commandArgs.push('--project-directory', projectRoot);
  }
  try {
    const result = await execute(executable, [...commandArgs, ...args], {
      cwd: projectRoot, env: { ...process.env, COMPOSE_DISABLE_ENV_FILE: '1' }, encoding: 'utf8', timeout, windowsHide: true, maxBuffer: 4 * 1024 * 1024,
    });
    return { ...result, code: 0 };
  } catch (error) {
    if (allowFailure && Number.isInteger(error.code)) {
      return { stdout: error.stdout || '', stderr: error.stderr || '', code: error.code };
    }
    // Child-process errors include command output. Keep it out of test logs.
    throw new Error(`Isolated auth Compose command failed (${args[0]}). Inspect ${projectName} locally.`);
  }
}

async function request(origin, endpoint, { method = 'GET', token, body, status = 200, code, headers = {} } = {}) {
  const url = new URL(endpoint, origin);
  check(allowedOrigins.has(url.origin) && url.hostname === '127.0.0.1', 'HTTP request must stay on a test loopback origin');
  const response = await fetch(url, {
    method,
    headers: {
      ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}),
      ...(token ? { Authorization: 'Bearer ' + token } : {}), ...headers,
    },
    body: body === undefined ? undefined : JSON.stringify(body),
    redirect: 'error', signal: AbortSignal.timeout(10000),
  });
  check(response.status === status, `${endpoint}: expected HTTP ${status}, received ${response.status}`);
  const data = await response.json();
  check(response.headers.get('cache-control') === 'no-store', `${endpoint}: no-store response`);
  if (status >= 400) {
    check(data.error?.code === code, `${endpoint}: expected diagnostic ${code}`);
    check(typeof data.error?.message === 'string' && data.error.message.length > 0, `${endpoint}: error message present`);
    check(typeof data.error?.requestId === 'string' && data.error.requestId.length > 0,
      `${endpoint}: error request ID present`);
    check(data.error.requestId === response.headers.get('x-request-id'), `${endpoint}: matching request ID`);
  }
  return data;
}

// MAX's signing algorithm sorts keys, decodes each value once, and joins with LF.
// Keep the numeric user ID as raw JSON text: Number cannot preserve signed int64.
function signedInitData({ id = maxId, date = Math.floor(Date.now() / 1000), user, extra = {} } = {}) {
  const parameters = {
    auth_date: String(date), query_id: 'auth-test-query',
    user: user ?? `{"id":${id},"first_name":"Auth + %26 =","last_name":"Тест"}`,
    ...extra,
  };
  const entries = Object.entries(parameters).sort(([left], [right]) => left < right ? -1 : left > right ? 1 : 0);
  const message = entries.map(([key, value]) => `${key}=${value}`).join('\n');
  const secret = createHmac('sha256', 'WebAppData').update(testBotToken).digest();
  const signature = createHmac('sha256', secret).update(message).digest('hex');
  return entries.map(([key, value]) => `${key}=${encodeURIComponent(value)}`).join('&') + '&hash=' + signature;
}

async function login(origin, endpoint, body) {
  const session = await request(origin, endpoint, { method: 'POST', body });
  check(typeof session.token === 'string' && /^[a-f0-9]{64}$/.test(session.token), 'Opaque session token issued');
  check(Date.parse(session.expiresAt) > Date.now(), 'Session expiry is in the future');
  check(typeof session.user?.id === 'string' && /^[a-f0-9-]{36}$/.test(session.user.id), 'Internal user UUID issued');
  sessions.push({ origin, token: session.token });
  return session;
}

async function acceptRules(origin, token, rules) {
  const accepted = await request(origin, '/api/community-rules/accept', {
    method: 'POST', token, body: { version: rules.version },
  });
  check(accepted.accepted === true && accepted.version === rules.version &&
    typeof accepted.acceptedAt === 'string', 'Current community rules are explicitly accepted');
  const status = await request(origin, '/api/community-rules/status', { token });
  check(status.accepted === true && status.version === rules.version && status.acceptedAt === accepted.acceptedAt,
    'Community rules acceptance persists');
}

async function databaseJson(sql) {
  const result = await compose(['exec', '-T', 'db', 'psql', '-X', '-q', '-A', '-t',
    '-v', 'ON_ERROR_STOP=1', '-U', 'auth_test', '-d', 'synapse_auth_test', '-c', sql]);
  return JSON.parse(result.stdout.trim());
}

async function verifyIsolation() {
  const config = JSON.parse((await compose(['config', '--format', 'json'])).stdout);
  check(config.name === projectName, 'Dedicated authentication test project');
  check(Object.keys(config.services).sort().join(',') === 'api,db,demo-fixture,init', 'Only isolated test services; no worker');
  check(config.networks.default.name === projectName + '_default', 'Dedicated test network');
  check(config.volumes.auth_test_postgres.name === 'synapse-auth-test-postgres', 'Dedicated PostgreSQL volume');
  check(config.services.db.environment.POSTGRES_DB === 'synapse_auth_test', 'Dedicated PostgreSQL database');
  for (const service of ['api', 'demo-fixture', 'init']) {
    const environment = config.services[service].environment;
    check(config.services[service].image === testImage, `${service}: uses the selected locally built image`);
    check(environment.MAX_BOT_TOKEN === testBotToken, `${service}: synthetic token only`);
    check(environment.MAX_API_BASE_URL === 'https://example.invalid', `${service}: non-routable MAX API URL`);
    check(environment.DATABASE_URL === 'postgresql://auth_test:auth-test-only-password@db:5432/synapse_auth_test',
      `${service}: dedicated database connection`);
  }
  check(config.services.api.environment.APP_ENV === 'production' && config.services.api.environment.DEMO_MODE === 'false',
    'Main test API uses production configuration with demo disabled');
  check(config.services.api.environment.APP_PUBLIC_URL === 'https://example.invalid' &&
    config.services.api.environment.MAX_WEBHOOK_SECRET.length >= 32, 'Production HTTPS and webhook configuration');
  for (const [service, port] of [['api', '8082'], ['demo-fixture', '8083']]) {
    const published = config.services[service].ports;
    check(published.length === 1 && published[0].host_ip === '127.0.0.1' && String(published[0].published) === port,
      `${service}: fixed loopback test port`);
  }
}

async function run() {
  check(process.argv.length === 2, 'This test accepts no URL or credential overrides');
  await verifyIsolation();
  let started = false;
  try {
    started = true;
    await compose(['up', '--no-build', '--wait', '--wait-timeout', '120'], { timeout: 150000 });
    await request(api, '/health/ready');
    const bootstrap = await request(api, '/api/bootstrap');
    check(bootstrap.mode === 'max' && bootstrap.maxConfigured === true, 'Production API exposes configured MAX mode');
    await request(api, '/api/profile', { status: 401, code: 'UNAUTHORIZED' });
    await request(api, '/api/auth/demo', { method: 'POST', body: { persona: 'anna' }, status: 403, code: 'DEMO_DISABLED' });

    const rules = await request(api, '/api/community-rules');
    check(typeof rules.version === 'string' && rules.version.length > 0, 'Public community rules have a current version');
    // The test volume survives reruns: use a fresh synthetic identity to exercise the initial gate.
    const rulesUser = await login(api, '/api/auth/max', {
      initData: signedInitData({ id: String(randomBytes(6).readUIntBE(0, 6) || 1) }),
    });
    const pendingRules = await request(api, '/api/community-rules/status', { token: rulesUser.token });
    check(pendingRules.accepted === false && pendingRules.acceptedAt === null, 'New MAX user has not accepted community rules');
    const pendingProfile = await request(api, '/api/profile', { token: rulesUser.token });
    const profileWrite = { method: 'PUT', token: rulesUser.token, body: scenarios.scenarios[0].profile,
      headers: { 'If-Match': '"' + pendingProfile.revision + '"' } };
    await request(api, '/api/profile', { ...profileWrite, status: 403, code: 'RULES_ACCEPTANCE_REQUIRED' });
    const unchangedProfile = await request(api, '/api/profile', { token: rulesUser.token });
    check(JSON.stringify(unchangedProfile) === JSON.stringify(pendingProfile), 'Blocked profile write changes no data or revision');
    await acceptRules(api, rulesUser.token, rules);
    const allowedProfile = await request(api, '/api/profile', profileWrite);
    check(allowedProfile.id === pendingProfile.id && allowedProfile.revision === pendingProfile.revision + 1,
      'Explicit rules acceptance permits the previously blocked profile write');
    console.log('PASS community rules gate profile writes until explicit acceptance.');

    const demo = await login(demoApi, '/api/auth/demo', { persona: 'anna' });
    check(demo.user.provenance === 'demo', 'Fixture API issued a real demo session');
    await request(demoApi, '/api/profile', { token: demo.token });
    await request(api, '/api/profile', { token: demo.token, status: 401, code: 'UNAUTHORIZED' });
    console.log('PASS production API rejects demo login and an existing demo session.');
    await acceptRules(demoApi, demo.token, rules);
    let demoProfile = await request(demoApi, '/api/profile', { token: demo.token });
    for (const scenario of scenarios.scenarios) {
      const saved = await request(demoApi, '/api/profile', {
        method: 'PUT', token: demo.token, body: scenario.profile,
        headers: { 'If-Match': '"' + demoProfile.revision + '"' },
      });
      const loaded = await request(demoApi, '/api/profile', { token: demo.token });
      check(saved.id === demoProfile.id && saved.revision === demoProfile.revision + 1, 'Scenario keeps one identity and advances revision');
      check(JSON.stringify(loaded) === JSON.stringify(saved), 'Scenario profile persists in PostgreSQL');
      check(loaded.competencies[0].topicId === scenario.profile.competencies[0].topicId, 'Specific domain topic persists');
      check(loaded.competencies[0].evidenceStatus === 'none', 'Scenario without evidence claims no verification');
      demoProfile = saved;
    }
    console.log('PASS profile save/read for five participants across education and career; synthetic scenarios only.');

    const catalogState = await databaseJson('SELECT row_to_json(s)::text FROM catalog_state s WHERE singleton');
    check(catalogState.version === scenarios.catalogVersion && /^[a-f0-9]{64}$/.test(catalogState.checksum), 'Seed records catalog version and SHA-256');
    try {
      await databaseJson("WITH changed AS (UPDATE catalog_state SET checksum=repeat('0',64) RETURNING singleton) SELECT to_json(count(*))::text FROM changed");
      await request(api, '/health/ready', { status: 503, code: 'NOT_READY' });
      await request(api, '/health/live');
    } finally {
      await compose(['run', '--rm', '--no-deps', '-T', 'init']);
    }
    await request(api, '/health/ready');
    const afterSeed = await request(demoApi, '/api/profile', { token: demo.token });
    check(JSON.stringify(afterSeed) === JSON.stringify(demoProfile), 'Repeated migrate/seed preserves the edited profile and revision');
    console.log('PASS readiness detects catalog mismatch; repeat migration/seed restores readiness and preserves profile.');


    const valid = signedInitData();
    const first = await login(api, '/api/auth/max', { initData: valid });
    const repeated = await login(api, '/api/auth/max', { initData: valid });
    const adjacent = await login(api, '/api/auth/max', { initData: signedInitData({ id: adjacentId }) });
    check(first.user.id === repeated.user.id, 'Repeated signed MAX login keeps the same internal user ID');
    check(first.token !== repeated.token, 'Repeated login issues an independent session');
    check(first.user.id !== adjacent.user.id, 'Adjacent int64 MAX IDs above Number.MAX_SAFE_INTEGER stay distinct');
    check(first.user.provenance === 'self_declared', 'MAX session has self-declared provenance');
    check(first.user.displayName === 'Auth + %26 = Тест', 'Percent-encoded Unicode and special characters survive exactly once');
    const profile = await request(api, '/api/profile', { token: first.token });
    check(profile.id === first.user.id && profile.provenance === 'self_declared', 'Bearer session accesses its PostgreSQL profile');
    const storedUsers = await databaseJson(`SELECT json_agg(row_to_json(u) ORDER BY max_user_id)::text FROM
      (SELECT id::text, max_user_id, provenance FROM users WHERE max_user_id IN ('${maxId}','${adjacentId}')) u`);
    check(storedUsers.length === 2, 'PostgreSQL contains exactly two distinct large MAX identities');
    check(storedUsers.some(user => user.max_user_id === maxId && user.id === first.user.id && user.provenance === 'self_declared'),
      'PostgreSQL preserves signed int64 maximum exactly');
    check(storedUsers.some(user => user.max_user_id === adjacentId && user.id === adjacent.user.id),
      'PostgreSQL preserves adjacent signed int64 exactly');
    const tokenHash = createHash('sha256').update(first.token).digest('hex');
    const storedSession = await databaseJson(`SELECT row_to_json(s)::text FROM
      (SELECT token_hash, user_id::text, expires_at > now() AS active FROM sessions WHERE token_hash='${tokenHash}') s`);
    check(storedSession?.token_hash === tokenHash && storedSession.user_id === first.user.id && storedSession.active === true,
      'PostgreSQL persists the session hash against the verified identity');
    console.log('PASS signed MAX login, exact int64 identity, stable users, and persisted session hash.');

    const invalidCases = [
      ['modified user', valid.replace('Auth', 'Evil'), 'INIT_DATA_INVALID'],
      ['expired auth date', signedInitData({ date: Math.floor(Date.now() / 1000) - 3700 }), 'INIT_DATA_EXPIRED'],
      ['future auth date', signedInitData({ date: Math.floor(Date.now() / 1000) + 300 }), 'INIT_DATA_EXPIRED'],
      ['duplicate hash', valid + '&hash=' + valid.split('&hash=')[1], 'INIT_DATA_INVALID'],
      ['duplicate auth date', valid + '&auth_date=1', 'INIT_DATA_INVALID'],
      ['duplicate user parameter', valid + '&user=%7B%7D', 'INIT_DATA_INVALID'],
      ['missing hash', valid.slice(0, valid.indexOf('&hash=')), 'INIT_DATA_INVALID'],
      ['encoded hash key alias', valid.replace('&hash=', '&%68ash='), 'INIT_DATA_INVALID'],
      ['malformed percent encoding', valid + '&extra=%ZZ', 'INIT_DATA_INVALID'],
      ['duplicate JSON user ID', signedInitData({ user: '{"id":1,"id":2}' }), 'INIT_DATA_INVALID'],
      ['signed int64 overflow', signedInitData({ id: '9223372036854775808' }), 'INIT_DATA_INVALID'],
      ['string user ID', signedInitData({ user: '{"id":"123"}' }), 'INIT_DATA_INVALID'],
      ['zero user ID', signedInitData({ id: '0' }), 'INIT_DATA_INVALID'],
    ];
    for (const [label, initData, code] of invalidCases) {
      await request(api, '/api/auth/max', { method: 'POST', body: { initData }, status: 401, code });
      console.log('PASS rejects ' + label + '.');
    }

    await compose(['restart', 'api']);
    for (let attempt = 0; ; attempt++) {
      try {
        await request(api, '/health/ready');
        break;
      } catch {
        if (attempt >= 29) throw new Error('Auth test API did not become ready after restart.');
        await new Promise(resolve => setTimeout(resolve, 500));
      }
    }
    const persisted = await request(api, '/api/profile', { token: first.token });
    check(persisted.id === profile.id, 'Session and profile remain usable after API restart');
    await request(api, '/api/auth/logout', { method: 'POST', token: first.token });
    await request(api, '/api/profile', { token: first.token, status: 401, code: 'UNAUTHORIZED' });
    await request(api, '/api/profile', { token: repeated.token });
    const removedSession = await databaseJson(`SELECT to_json(count(*))::text FROM sessions WHERE token_hash='${tokenHash}'`);
    check(removedSession === 0, 'Logout removes the persisted session');
    console.log('PASS sessions survive API restart; logout revokes only the selected session.');

    // read_config runs before every command. Seed exits promptly even if this guard regresses.
    const forbidden = await compose(['run', '--rm', '--no-deps', '-T', '-e', 'DEMO_MODE=true',
      'api', '/app/bin/max-help', 'seed'], { allowFailure: true });
    check(forbidden.code === 1 && (forbidden.stdout + forbidden.stderr).includes('STARTUP_OR_COMMAND_FAILED'),
      'Production refuses to start when DEMO_MODE=true');
    console.log('PASS production configuration rejects DEMO_MODE=true.');
  } finally {
    if (started) {
      for (const session of sessions) {
        try {
          await request(session.origin, '/api/auth/logout', { method: 'POST', token: session.token });
        } catch {
          // The whole test stack is stopped below even when HTTP cleanup is unavailable.
        }
      }
      await compose(['down', '--timeout', '10']);
    }
  }
  console.log(`PASS ${checks} auth integration assertions; isolated stack stopped, test volume retained.`);
}

run().catch(error => {
  // Do not dump HTTP bodies, signed initData, session tokens, or subprocess output.
  console.error('FAIL auth integration: ' + (error instanceof Error ? error.message : 'unknown error'));
  process.exitCode = 1;
});
