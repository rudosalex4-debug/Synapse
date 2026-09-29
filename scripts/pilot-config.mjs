// Offline validation inside the tools container. Never print raw environment values.
import { readFileSync } from 'node:fs';
import { timingSafeEqual } from 'node:crypto';
import { X509Certificate } from 'node:crypto';

class ConfigError extends Error {}
const fail = message => { throw new ConfigError(message); };
try {
  const env = process.env;
  const args = process.argv.slice(2);
  if (args.length && (args.length !== 1 || args[0] !== '--compare-live')) fail('Unknown pilot configuration operation.');
  if (args[0] === '--compare-live') {
    // Input comes directly from docker inspect through a pipe, never a log or temp file.
    const raw = readFileSync(0, 'utf8');
    if (raw.length > 256 * 1024) fail('Live container configuration exceeds size limit.');
    const entries = JSON.parse(raw);
    if (!Array.isArray(entries) || entries.some(item => typeof item !== 'string' || !item.includes('='))) {
      fail('Could not read live container configuration.');
    }
    const live = Object.fromEntries(entries.map(item => [item.slice(0, item.indexOf('=')), item.slice(item.indexOf('=') + 1)]));
    const keys = ['APP_ENV', 'DEMO_MODE', 'PILOT_MODE', 'PILOT_ALLOWED_MAX_IDS', 'APP_PUBLIC_URL',
      'MAX_BOT_TOKEN', 'MAX_BOT_USERNAME', 'MAX_WEBHOOK_SECRET', 'MAX_API_BASE_URL', 'DATABASE_URL', 'MODERATOR_MAX_IDS',
      'MAX_PRODUCT_NOTIFICATIONS_ENABLED'];
    for (const key of keys) {
      const left = Buffer.from(live[key] ?? ''), right = Buffer.from(env[key] ?? '');
      if (left.length !== right.length || !timingSafeEqual(left, right)) {
        fail('Live container settings differ from .env.pilot.local. Run deploy --no-build before connect; values hidden.');
      }
    }
    console.log('Live container and selected pilot settings match. Values hidden.');
    process.exit(0);
  }
  if (env.APP_ENV !== 'production' || env.DEMO_MODE !== 'false' || !['true', 'false'].includes(env.PILOT_MODE)) {
    fail('MAX release requires APP_ENV=production, DEMO_MODE=false and PILOT_MODE=true or false.');
  }
  const url = new URL(env.APP_PUBLIC_URL || '');
  if (url.protocol !== 'https:' || url.port || url.username || url.password || url.search || url.hash || url.pathname !== '/' ||
      !/^(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z]{2,63}$/i.test(url.hostname) ||
      /\.(local|localhost|test|invalid|example)$/.test(url.hostname) || url.hostname === 'pilot.example.org') {
    fail('Set APP_PUBLIC_URL to the actual public HTTPS origin, without a path or credentials.');
  }
  if (!/^[A-Za-z0-9_-]{20,4096}$/.test(env.MAX_BOT_TOKEN || '') || env.MAX_BOT_TOKEN.startsWith('REPLACE_')) {
    fail('Set a real MAX_BOT_TOKEN in .env.pilot.local.');
  }
  if (!/^[A-Za-z0-9_]{1,128}$/.test(env.MAX_BOT_USERNAME || '')) fail('Set MAX_BOT_USERNAME without @ or a URL.');
  if (!/^[A-Za-z0-9_-]{32,256}$/.test(env.MAX_WEBHOOK_SECRET || '') || env.MAX_WEBHOOK_SECRET.startsWith('REPLACE_')) {
    fail('Set the generated MAX_WEBHOOK_SECRET; preserve the existing secret when updating.');
  }
  const db = new URL(env.DATABASE_URL || '');
  if (db.protocol !== 'postgresql:' || db.hostname !== 'db' || db.username !== 'pilot' || db.pathname !== '/synapse_pilot' ||
      db.search || db.hash || !db.password || db.password.startsWith('REPLACE_')) {
    fail('Invalid pilot DATABASE_URL; check POSTGRES_PASSWORD without replacing an existing database password.');
  }
  if (!['true', 'false'].includes(env.MAX_PRODUCT_NOTIFICATIONS_ENABLED)) {
    fail('MAX_PRODUCT_NOTIFICATIONS_ENABLED must be exactly true or false.');
  }
  const rawAllowed = env.PILOT_ALLOWED_MAX_IDS || '';
  if (rawAllowed.length > 12000) fail('PILOT_ALLOWED_MAX_IDS is too long.');
  const allowed = rawAllowed === '' ? [] : rawAllowed.split(',').map(value => value.replace(/^[ \t]+|[ \t]+$/g, ''));
  if (allowed.length > 500 || allowed.some(value => !/^[1-9][0-9]{0,18}$/.test(value) || BigInt(value) > 9223372036854775807n)) {
    fail('PILOT_ALLOWED_MAX_IDS must contain positive numeric MAX IDs separated by commas.');
  }
  const rawModerators = env.MODERATOR_MAX_IDS || '';
  const moderators = rawModerators === '' ? [] : rawModerators.split(',').map(value => value.replace(/^[ \t]+|[ \t]+$/g, ''));
  if (rawModerators.length > 1200 || new Set(moderators).size > 50 || moderators.some(value => !/^[1-9][0-9]{0,18}$/.test(value) || BigInt(value) > 9223372036854775807n)) {
    fail('MODERATOR_MAX_IDS must contain at most 50 positive numeric MAX IDs, without empty entries.');
  }
  const ca = readFileSync(env.MAX_CA_FILE || '');
  const certificates = ca.toString('ascii').match(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g);
  if (ca.length > 128 * 1024 || !certificates?.length) fail('Invalid MAX CA bundle.');
  for (const certificate of certificates) new X509Certificate(certificate);
  console.log('Pilot settings and PEM certificates are readable. Secrets are hidden.');
  console.log(env.PILOT_MODE === 'true' ? 'Access: closed pilot. Allowed participant count: ' + new Set(allowed).size :
    'Access: all authenticated MAX users. The participant allowlist is ignored; moderator roles stay separate.');
  if (env.PILOT_MODE === 'true' && !allowed.length) console.log('PILOT_ALLOWED_MAX_IDS is empty: all accounts are denied, including bot commands.');
  console.log('Product alerts: ' + (env.MAX_PRODUCT_NOTIFICATIONS_ENABLED === 'true' ? 'available with per-account opt-in.' : 'globally disabled.'));
} catch (error) {
  console.error(error instanceof ConfigError ? error.message :
    'Pilot configuration is invalid or CA is unreadable. Check .env.pilot.local and private/max-ca.pem; details hidden.');
  process.exitCode = 1;
}
