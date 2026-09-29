import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { loadEnvFile } from 'node:process';

const production = process.argv.includes('--production');
if (process.argv.slice(2).some((arg) => arg !== '--production')) {
  console.error('Usage: node scripts/doctor.mjs [--production]');
  process.exit(1);
}
const envFile = production ? '.env.production' : '.env';
if (existsSync(envFile)) {
  try { loadEnvFile(envFile); }
  catch { console.error('FAIL configuration: cannot read environment file; values hidden'); process.exit(1); }
}
let failures = 0;
function report(ok, label, note = '') {
  console.log(`${ok ? 'OK' : 'FAIL'} ${label}${note ? ': ' + note : ''}`);
  if (!ok) failures++;
}
function command(label, executable, args) {
  const result = spawnSync(executable, args, { encoding: 'utf8', timeout: 15000, windowsHide: true });
  report(result.status === 0, label, result.status === 0 ? 'available' : 'unavailable; check installation/service');
}
report(Number(process.versions.node.split('.')[0]) === 24, 'Node.js 24 (frontend and helper scripts)');
report(existsSync('pnpm-lock.yaml'), 'pnpm lockfile');
report(existsSync('apps/backend/CMakeLists.txt'), 'C++ backend sources');
console.log(`INFO local frontend build: ${existsSync('apps/miniapp/dist/index.html') ? 'present' : 'absent; Docker builds it inside the image'}`);
command('Docker CLI', 'docker', ['--version']);
command('Docker daemon', 'docker', ['info', '--format', '{{.ServerVersion}}']);
command('Docker Compose', 'docker', ['compose', 'version']);
command('Local Compose configuration', 'docker', ['compose', 'config', '--quiet']);
report(process.env.NODE_TLS_REJECT_UNAUTHORIZED !== '0', 'TLS certificate verification enabled');
for (const key of ['MAX_BOT_TOKEN', 'MAX_BOT_USERNAME', 'MAX_WEBHOOK_SECRET']) {
  const present = Boolean(process.env[key]?.trim());
  if (production) report(present, key, present ? 'set; value hidden' : 'missing');
  else console.log(`INFO ${key}: ${present ? 'set; value hidden' : 'not set; local demo needs no real MAX connection'}`);
}
if (production) {
  report(process.env.APP_ENV === 'production', 'APP_ENV=production');
  report(process.env.DEMO_MODE === 'false', 'DEMO_MODE=false');
  report(/^[a-zA-Z0-9_-]{32,256}$/.test(process.env.MAX_WEBHOOK_SECRET ?? '') &&
    process.env.MAX_WEBHOOK_SECRET !== 'local-webhook-test-only', 'random webhook secret', '32–256 allowed characters required');
  let httpsOrigin = false;
  try {
    const url = new URL(process.env.APP_PUBLIC_URL || '');
    httpsOrigin = url.protocol === 'https:' && !url.username && !url.password && !url.port &&
      url.pathname === '/' && !url.search && !url.hash &&
      /^(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z]{2,63}$/i.test(url.hostname) &&
      !/\.(local|localhost|test|invalid|example)$/.test(url.hostname);
  } catch { /* Report without exposing the value. */ }
  report(httpsOrigin, 'public HTTPS origin', 'syntax only; endpoint not contacted');
  report(Boolean(process.env.DATABASE_URL?.trim()) && !process.env.DATABASE_URL.includes('local-development-only'),
    'production database configuration', 'local example password must be replaced');
  console.log('INFO production mode checks settings only; public deployment is not configured or verified.');
}
console.log('No external API requests made. No credentials printed.');
process.exitCode = failures ? 1 : 0;
