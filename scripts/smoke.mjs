import assert from 'node:assert/strict';

const base = new URL(process.argv[2] || 'http://127.0.0.1:8080');
if (process.argv.length > 3 || !['http:', 'https:'].includes(base.protocol) ||
    !['localhost', '127.0.0.1', '[::1]'].includes(base.hostname) ||
    base.username || base.password || base.search || base.hash || base.pathname !== '/') {
  throw new Error('Smoke checks only accept a loopback origin, e.g. http://127.0.0.1:8080.');
}
async function get(path, status = 200) {
  const response = await fetch(new URL(path, base), { signal: AbortSignal.timeout(5000), redirect: 'error' });
  assert.equal(response.status, status, `${path}: unexpected HTTP status`);
  assert.match(response.headers.get('content-type') || '', /application\/json/);
  const body = await response.json();
  assert.ok(body && typeof body === 'object' && !Array.isArray(body), `${path}: JSON object expected`);
  console.log(`PASS ${path}`);
  return body;
}
await get('/health/live');
await get('/health/ready');
const bootstrap = await get('/api/bootstrap');
assert.ok(['demo', 'max'].includes(bootstrap.mode), 'bootstrap mode');
assert.equal(typeof bootstrap.maxConfigured, 'boolean', 'bootstrap MAX status');
assert.equal(typeof bootstrap.version, 'string', 'bootstrap version');
const taxonomy = await get('/api/taxonomy');
assert.ok(Array.isArray(taxonomy.topics) && taxonomy.topics.length > 0, 'taxonomy topics');
const unauthenticated = await get('/api/profile', 401);
assert.equal(typeof unauthenticated.error?.code, 'string', 'unauthenticated error code');
const app = await fetch(base, { signal: AbortSignal.timeout(5000), redirect: 'error' });
assert.equal(app.status, 200, 'mini app: HTTP 200 expected');
assert.match(app.headers.get('content-type') || '', /text\/html/);
assert.match(await app.text(), /<div[^>]+id=["']root["']/);
console.log('PASS mini app HTML');
console.log('Local read-only HTTP checks passed. This does not prove a MAX web/mobile launch.');
