// Synthetic webhook -> durable worker -> production auth -> mini HTTP workflow.
// Owns only compose.bot-mini-test.yaml and deletes only its disposable test volume.
// No .env/private files, external MAX calls, browser or live MAX assertions.
import assert from 'node:assert/strict';
import { createHmac, randomBytes, randomUUID } from 'node:crypto';
import { execFile } from 'node:child_process';
import { readdir } from 'node:fs/promises';
import { promisify } from 'node:util';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const execute = promisify(execFile);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const project = 'synapse-bot-mini-test', base = 'http://127.0.0.1:8086';
const token = 'local-test-token-not-a-real-bot-token';
const webhookSecret = 'bot-mini-test-only-secret-at-least-32-characters';
let checks = 0, started = false, timestamp = Date.now();
function check(condition, message) { assert.ok(condition, message); checks++; }
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const literal = value => "'" + String(value).replaceAll("'", "''") + "'";

async function compose(args, timeout = 60000) {
  let bin = 'docker';
  let prefix = ['compose', '-p', project, '-f', path.join(root, 'compose.bot-mini-test.yaml')];
  if (process.platform === 'win32') {
    const match = /^([a-zA-Z]):\/(.+)$/.exec(root.replaceAll('\\', '/'));
    if (!match) throw Error('Run this test within WSL for UNC paths.');
    const linux = '/mnt/' + match[1].toLowerCase() + '/' + match[2];
    bin = 'wsl';
    prefix = ['-d', process.env.SYNAPSE_WSL_DISTRO || 'kali-linux', '--', 'env',
      'COMPOSE_DISABLE_ENV_FILE=1', 'SYNAPSE_TEST_IMAGE=' + (process.env.SYNAPSE_TEST_IMAGE || 'synapse:day23-check'),
      'docker', 'compose', '-p', project, '--project-directory', linux,
      '-f', linux + '/compose.bot-mini-test.yaml'];
  }
  try {
    return await execute(bin, [...prefix, ...args], { cwd: root, encoding: 'utf8', windowsHide: true,
      env: { ...process.env, COMPOSE_DISABLE_ENV_FILE: '1' }, timeout, maxBuffer: 2 * 1024 * 1024 });
  } catch {
    // Never print captured commands/DB response bodies, which can contain synthetic auth data.
    throw Error('Isolated test Compose operation failed: ' + args[0]);
  }
}
async function sql(query) {
  return (await compose(['exec', '-T', 'db', 'psql', '-X', '-v', 'ON_ERROR_STOP=1',
    '-U', 'bot_mini_test', '-d', 'synapse_bot_mini_test', '-At', '-c', query])).stdout.trim();
}
async function rows(query) { return JSON.parse(await sql('SELECT COALESCE(json_agg(t),\'[]\'::json) FROM (' + query + ') t')); }
async function api(actor, endpoint, { method = 'GET', body, status = 200, revision, key, headers = {} } = {}) {
  const response = await fetch(base + endpoint, { method, headers: {
    ...(actor ? { Authorization: 'Bearer ' + actor.token } : {}),
    ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}),
    ...(revision !== undefined ? { 'If-Match': '"' + revision + '"' } : {}),
    ...(key ? { 'Idempotency-Key': key } : {}), ...headers },
    body: body === undefined ? undefined : JSON.stringify(body), redirect: 'error', signal: AbortSignal.timeout(12000) });
  const data = await response.json();
  check((Array.isArray(status) ? status : [status]).includes(response.status),
    method + ' ' + endpoint + ': expected ' + status + ', got ' + response.status + ' ' + (data.error?.code ?? ''));
  if (response.status >= 400) check(typeof data.error?.requestId === 'string', 'Error includes a request ID');
  return data;
}
const post = (actor, endpoint, body = {}, options = {}) => api(actor, endpoint,
  { method: 'POST', body, key: randomUUID(), ...options });

function person(name) {
  const id = Number.parseInt(randomBytes(6).toString('hex'), 16);
  return { id, chatId: id + 1, name };
}
function updateFor(who, text, callback = false) {
  const id = randomUUID(), user = { user_id: who.id, first_name: who.name, is_bot: false };
  const message = { recipient: { chat_id: who.chatId, chat_type: 'dialog' },
    sender: user, body: { mid: id, text } };
  const update = { update_type: callback ? 'message_callback' : 'message_created', timestamp: ++timestamp, message };
  if (callback) update.callback = { callback_id: id, user, payload: text };
  return { update, eventKey: (callback ? 'callback:' : 'message:') + id };
}
async function deliver(event) {
  await api(null, '/webhooks/max', { method: 'POST', body: event.update,
    headers: { 'X-Max-Bot-Api-Secret': webhookSecret } });
  const deadline = Date.now() + 18000;
  while (Date.now() < deadline) {
    const found = await rows('SELECT i.status AS inbox_status,o.payload FROM bot_inbox i LEFT JOIN outbox o ON o.event_key=i.event_key WHERE i.event_key=' + literal(event.eventKey));
    if (found[0]?.inbox_status === 'dead') throw Error('Synthetic bot event became dead.');
    if (found[0]?.inbox_status === 'done') {
      check(Boolean(found[0].payload), 'Completed bot event has a durable response');
      const response = found[0].payload;
      return response.kind === 'answer' ? response.body.message : response.body;
    }
    await pause(200);
  }
  throw Error('Timed out waiting for the isolated inbox worker.');
}
const say = (who, text) => deliver(updateFor(who, text));
const click = (who, payload) => deliver(updateFor(who, payload, true));
function buttons(message) { return (message.attachments ?? []).flatMap(a => a.payload?.buttons ?? []).flat(); }
function choice(message, suffix) {
  const button = buttons(message).find(b => typeof b.payload === 'string' && b.payload.endsWith(':' + suffix));
  check(Boolean(button), 'Current bot response provides action ' + suffix);
  return button.payload;
}
async function selectTopic(who, message, topicId) {
  const pieces = topicId.split('.');
  for (let i = 1; i <= pieces.length; i++) message = await click(who, choice(message, 'topic:' + pieces.slice(0, i).join('.')));
  return click(who, choice(message, 'topic_done'));
}
async function acceptRules(who) {
  const message = await say(who, '/start');
  const accept = buttons(message).find(b => b.payload?.startsWith('rules:accept:'));
  check(Boolean(accept), 'New bot participant receives current community rules and explicit acceptance');
  const blocked = await say(who, '/knowledge');
  check(buttons(blocked).some(b => b.payload === accept.payload), 'Knowledge input requires explicit rules acceptance');
  const accepted = await click(who, accept.payload);
  check(accepted.text.includes('Правила сообщества приняты'), 'Bot records explicit rules acceptance');
}
async function selectContext(who, message, facetId, valueId) {
  message = await click(who, choice(message, 'facet:' + facetId));
  message = await click(who, choice(message, 'facet_value:' + valueId));
  message = await click(who, choice(message, 'facet_done'));
  return click(who, choice(message, 'context_done'));
}
async function addKnowledge(who, topicId, facetId, valueId) {
  let message = await say(who, '/knowledge');
  message = await selectTopic(who, message, topicId);
  message = await selectContext(who, message, facetId, valueId);
  message = await say(who, 'I can share my educational or career experience and explain the limits of it.');
  message = await click(who, choice(message, 'experience:practice'));
  message = await click(who, choice(message, 'available:yes'));
  const save = updateFor(who, choice(message, 'save'), true);
  message = await deliver(save);
  check(message.text.includes('Знание сохранено'), 'Bot saves a competence');
  await deliver(save); // Same authenticated webhook ID must not write twice.
  return save;
}
async function login(who) {
  const data = { auth_date: String(Math.floor(Date.now() / 1000)),
    user: JSON.stringify({ id: who.id, first_name: who.name }) };
  const checkString = Object.entries(data).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0).map(([k, v]) => k + '=' + v).join('\n');
  const secret = createHmac('sha256', 'WebAppData').update(token).digest();
  const hash = createHmac('sha256', secret).update(checkString).digest('hex');
  // MAX initData uses URI-component decoding: spaces must be %20, not form '+' encoding.
  const initData = Object.entries({ ...data, hash })
    .map(([key, value]) => `${key}=${encodeURIComponent(value)}`).join('&');
  return api(null, '/api/auth/max', { method: 'POST', body: { initData } });
}
const editableRequest = r => ({ title: r.title, body: r.body, topicId: r.topicId,
  learningGoal: r.learningGoal, facets: r.facets, requiredFacets: r.requiredFacets,
  taxonomyVersion: r.taxonomyVersion, ...(r.attempt == null ? {} : { attempt: r.attempt }) });
async function notificationCount(actor, kind) {
  return Number(await sql('SELECT count(*) FROM outbox WHERE product_context->>\'recipientId\'=' + literal(actor.user.id) +
    ' AND product_context->>\'kind\'=' + literal(kind)));
}

try {
  const config = JSON.parse((await compose(['config', '--format', 'json'])).stdout);
  check(config.name === project, 'Uses the dedicated disposable project');
  check(config.services.api.environment.MAX_BOT_TOKEN === token &&
    config.services.worker.environment.MAX_DELIVERY_ENABLED === 'false' &&
    config.services.worker.environment.MAX_API_BASE_URL === 'https://example.invalid' &&
    config.services.api.environment.DEMO_MODE === 'false', 'Production auth with synthetic credentials and no MAX delivery');
  check(config.volumes.bot_mini_test_postgres.name === 'synapse-bot-mini-test-postgres', 'Database volume is test-only');
  started = true;
  await compose(['up', '--no-build', '--wait', '--wait-timeout', '120'], 150000);
  await api(null, '/health/ready');
  const migrations = await rows('SELECT version FROM schema_migrations ORDER BY version');
  const expectedMigrations = (await readdir(path.join(root, 'db/migrations'))).filter(name => name.endsWith('.sql')).sort();
  check(JSON.stringify(migrations.map(r => r.version)) === JSON.stringify(expectedMigrations), 'Every shipped migration is installed exactly once');

  // Readiness must fail if the bot form migration is missing from the ledger.
  const [ledger] = await rows("SELECT version,checksum,applied_at FROM schema_migrations WHERE version='006_bot_forms.sql'");
  try {
    await sql("DELETE FROM schema_migrations WHERE version='006_bot_forms.sql'");
    const notReady = await api(null, '/health/ready', { status: 503 });
    check(notReady.error.code === 'NOT_READY', 'Readiness includes the colleague bot migration');
  } finally {
    await sql('INSERT INTO schema_migrations(version,checksum,applied_at) VALUES (' +
      [ledger.version, ledger.checksum, ledger.applied_at].map(literal).join(',') + ') ON CONFLICT(version) DO NOTHING');
  }
  await api(null, '/health/ready');
  const page = await fetch(base + '/', { signal: AbortSignal.timeout(12000) });
  check(page.status === 200 && (await page.text()).includes('id="root"'), 'Same API serves the compiled mini application');
  await api(null, '/api/profile', { status: 401 });
  await api(null, '/webhooks/max', { method: 'POST', body: { update_type: 'unknown' }, status: 403 });

  const author = person('Cross channel author'), helper = person('Cross channel helper'), outsider = person('Cross channel outsider');
  await acceptRules(author);
  await acceptRules(helper);
  await addKnowledge(author, 'career.application.cv', 'company', 'yandex');
  await addKnowledge(helper, 'pathways.admissions.program_choice', 'organization', 'itmo');
  const a = await login(author), h = await login(helper), o = await login(outsider);
  const rules = await api(null, '/api/community-rules');
  for (const actor of [a, h]) {
    const status = await api(actor, '/api/community-rules/status');
    check(status.accepted && status.version === rules.version && typeof status.acceptedAt === 'string', 'Bot acceptance is visible to the same MAX-authenticated mini user');
  }
  await post(o, '/api/community-rules/accept', { version: rules.version });
  const authorProfile = await api(a, '/api/profile'), helperProfile = await api(h, '/api/profile');
  check(authorProfile.competencies.length === 1 && authorProfile.competencies[0].topicId === 'career.application.cv', 'Bot competence appears in the same MAX-authenticated mini profile exactly once');
  check(helperProfile.competencies.length === 1 && helperProfile.competencies[0].topicId === 'pathways.admissions.program_choice', 'Second bot participant has a distinct shared profile');
  check(authorProfile.competencies[0].facets.company?.[0] === 'yandex' && helperProfile.competencies[0].facets.organization?.[0] === 'itmo', 'Bot context selections persist in the shared mini profiles');
  check(authorProfile.id !== helperProfile.id && authorProfile.id === a.user.id, 'Cross-channel identity is one profile per verified MAX ID');
  const initialPreference = await api(a, '/api/notification-settings');
  check(!initialPreference.enabled && initialPreference.botStarted && initialPreference.deliveryAvailable, 'Notifications default off, bot startup is visible, and the configured notification channel is available');
  await api(a, '/api/notification-settings', { method: 'PUT', body: { enabled: true } });
  await api(h, '/api/notification-settings', { method: 'PUT', body: { enabled: true } });

  let message = await say(author, '/ask');
  const oldFormCancel = choice(message, 'cancel');
  message = await say(author, 'How can I compare university degree programs?');
  message = await click(author, oldFormCancel);
  check(message.text.includes('Эта кнопка устарела'), 'Old form callback cannot cancel or change a later step');
  message = await say(author, 'I am comparing university degree programs and want to understand how to compare their curricula and practical experience opportunities.');
  message = await selectTopic(author, message, 'pathways.admissions.program_choice');
  message = await selectContext(author, message, 'organization', 'itmo');
  message = await click(author, choice(message, 'goal:learning_path'));
  const saveRequest = updateFor(author, choice(message, 'save'), true);
  message = await deliver(saveRequest);
  const publishButton = buttons(message).find(b => b.payload?.startsWith('publish:'));
  check(Boolean(publishButton), 'Saving in the bot requires a separate publication action');
  const originalPublish = publishButton.payload, requestId = originalPublish.split(':')[1];
  await deliver(saveRequest);
  let request = await api(a, '/api/requests/' + requestId);
  check(request.status === 'draft' && request.authorId === a.user.id && request.topicId === 'pathways.admissions.program_choice', 'Bot draft is owned by the same mini user');
  check(request.facets.organization?.[0] === 'itmo' && request.learningGoal === 'learning_path', 'Bot draft retains selected context and educational goal');
  check((await api(a, '/api/requests?scope=mine')).items.length === 1, 'Duplicate save webhook creates only one request');
  await api(o, '/api/requests/' + requestId, { status: 404 });
  const reach = await api(a, '/api/requests/' + requestId + '/reach');
  check(reach.status === 'available' && reach.requestRevision === request.revision, 'Reach preview sees the competence created in the bot');
  check(!JSON.stringify(reach).includes(helperProfile.id), 'Reach preview does not disclose candidate identity');
  await api(o, '/api/requests/' + requestId + '/reach', { status: [403, 404] });

  request = await api(a, '/api/requests/' + requestId, { method: 'PUT', revision: request.revision,
    body: { ...editableRequest(request), attempt: 'I compared the course lists and want to understand how much practical work each program includes.' } });
  message = await click(author, originalPublish);
  check(message.text.includes('Вопрос изменился'), 'Stale bot publication button detects a mini edit');
  check((await api(a, '/api/requests/' + requestId)).status === 'draft', 'Stale bot button cannot publish modified content');
  message = await click(author, 'request:' + requestId);
  const currentPublish = buttons(message).find(b => b.payload?.startsWith('publish:'))?.payload;
  check(Boolean(currentPublish) && currentPublish !== originalPublish, 'Bot refresh provides the current draft revision');
  const publishEvent = updateFor(author, currentPublish, true);
  await deliver(publishEvent);
  await deliver(publishEvent);
  check((await api(a, '/api/requests/' + requestId)).status === 'open', 'Explicit current bot publication opens the mini request');
  const matched = (await api(h, '/api/requests?scope=feed')).items.find(r => r.id === requestId);
  check(matched?.match?.score >= 70 && matched.match.explanation?.topicRelation === 'exact', 'Mini helper feed matches the bot competence with a topic explanation');
  check(!(await api(a, '/api/requests?scope=feed')).items.some(r => r.id === requestId), 'Author cannot be their own helper');

  const offerKey = randomUUID();
  const offer = await post(h, '/api/requests/' + requestId + '/offers', { message: 'I can share my experience comparing course plans.' }, { status: 201, key: offerKey });
  await post(h, '/api/requests/' + requestId + '/offers', { message: 'I can share my experience comparing course plans.' }, { status: 201, key: offerKey });
  check(await notificationCount(a, 'offer_received') === 1, 'Offer creates one opt-in bot notification even after API retry');
  check((await api(a, '/api/requests/' + requestId + '/offers')).items.length === 1, 'Author sees the mini offer once');
  const chat = await post(a, '/api/offers/' + offer.id + '/accept');
  check(chat.status === 'active' && chat.authorId === a.user.id && chat.helperId === h.user.id, 'Selecting the mini offer creates the correct private chat');
  check(await notificationCount(h, 'helper_selected') === 1, 'Chosen helper receives a durable bot notification');
  await api(o, '/api/conversations/' + chat.id + '/messages', { status: 404 });
  const clientMessageId = randomUUID();
  const sent = await post(a, '/api/conversations/' + chat.id + '/messages', { clientMessageId, text: 'How do I compare the practical parts of these programs?' }, { status: 201 });
  const resent = await post(a, '/api/conversations/' + chat.id + '/messages', { clientMessageId, text: 'How do I compare the practical parts of these programs?' }, { status: 201 });
  check(sent.id === resent.id, 'Retry does not duplicate the private message');
  await post(a, '/api/conversations/' + chat.id + '/messages', { clientMessageId: randomUUID(), text: 'Can we start with the published course plans?' }, { status: 201 });
  check(await notificationCount(h, 'message_received') === 1, 'Nearby messages coalesce into one bot notification');
  await post(h, '/api/conversations/' + chat.id + '/messages', { clientMessageId: randomUUID(), text: 'Compare the project courses and practice requirements, then verify the details with each university.' }, { status: 201 });
  check((await api(a, '/api/conversations/' + chat.id + '/messages?after=0&limit=50')).items.length === 3, 'Both channels share the three persisted private messages');
  const nextStep = 'Compare the published course plans and ask each university about practice opportunities.';
  const closed = await post(a, '/api/conversations/' + chat.id + '/close', { outcome: 'helpful', comment: 'I understand which parts of the programs to compare.', nextStep });
  check(closed.status === 'closed' && closed.nextStep === nextStep, 'Author closes the interaction with a persisted learning next step');
  check((await api(a, '/api/requests/' + requestId)).status === 'resolved', 'Successful mini conversation resolves the bot-created question');
  await post(h, '/api/conversations/' + chat.id + '/messages', { clientMessageId: randomUUID(), text: 'Late message.' }, { status: 409 });
  message = await say(author, '/requests');
  check(message.text.includes('Завершён с результатом'), 'Bot request list reflects completion in mini');
  message = await say(author, '/stats');
  check(message.text.includes('Завершён с результатом: 1') && message.text.includes('Откликов за всё время: 1') &&
    message.text.includes('Выбрано помощников: 1'), 'Bot statistics count the mini outcome, offer and selected helper');

  await api(a, '/api/notification-settings', { method: 'PUT', body: { enabled: false } });
  const pending = Number(await sql("SELECT count(*) FROM outbox WHERE status='pending' AND product_context->>'recipientId'=" + literal(a.user.id)));
  check(pending === 0, 'Disabling notifications cancels this user pending product messages');
  check((await api(h, '/api/notification-settings')).enabled, 'Notification preference is isolated per user');
  check(Number(await sql('SELECT count(*) FROM outbox WHERE attempts<>0')) === 0, 'Worker processed bot forms without any external MAX delivery attempt');
  console.log('PASS ' + checks + ' bot-mini assertions: all shipped migrations/readiness, explicit rules acceptance, synthetic webhook worker, shared identity/profile/draft, replay protection, stale publish, reach/matching, offers/private chat/outcome, bot statistics, notification opt-in/deduplication/cancellation.');
} finally {
  if (started) await compose(['down', '--volumes', '--remove-orphans', '--timeout', '10']);
}
