// Explicit offline change of access mode. Never print secret values.
import { readFileSync, writeFileSync, renameSync, unlinkSync, existsSync, lstatSync,
  mkdirSync, chmodSync, openSync, fsyncSync, closeSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { parseEnv } from 'node:util';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

class AccessError extends Error {}
const fail = message => { throw new AccessError(message); };
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const target = path.join(root, '.env.pilot.local');
let temporary;
try {
  const args = process.argv.slice(2);
  if (args.length !== 1 || !['public', 'closed'].includes(args[0])) {
    fail('Usage: node scripts/pilot-access.mjs public|closed');
  }
  const key = 'PILOT_MODE';
  const value = args[0] === 'public' ? 'false' : 'true';
  const description = 'Access mode: ' + args[0];
  if (!existsSync(target) || !lstatSync(target).isFile() || lstatSync(target).isSymbolicLink()) {
    fail('Expected a regular .env.pilot.local file; symbolic links are not changed.');
  }
  if (lstatSync(target).size > 128 * 1024) fail('Settings file exceeds the size limit.');
  const original = readFileSync(target);
  const source = original.toString('utf8');
  if (!Buffer.from(source, 'utf8').equals(original) || source.includes('\0')) {
    fail('Settings must be a UTF-8 text file. No changes were made.');
  }
  // Preserve every other line, including quoted credentials and comments.
  const pattern = new RegExp('^[ \\t]*(?:export[ \\t]+)?' + key + '[ \\t]*=[^\\r\\n]*', 'gm');
  const matches = [...source.matchAll(pattern)];
  if (matches.length > 1) fail('Duplicate ' + key + ' entries: resolve them locally before changing settings.');
  const setting = key + '=' + value;
  const newline = source.includes('\r\n') ? '\r\n' : '\n';
  const updated = matches.length ? source.replace(pattern, setting) :
    source + (source.endsWith('\n') || !source ? '' : newline) + setting + newline;
  // A key-like line inside a multiline quoted value must never change a secret.
  const before = parseEnv(source), after = parseEnv(updated);
  if (after[key] !== value || Object.keys({ ...before, ...after }).some(name => name !== key && before[name] !== after[name])) {
    fail('Settings contain ambiguous or multiline assignments. Change the requested setting with a local editor.');
  }
  if (updated === source) {
    console.log(description + '. Already configured; file was not rewritten. Deploy to apply it to running containers.');
    process.exit(0);
  }
  for (const directory of [path.join(root, 'private'), path.join(root, 'private', 'backups')]) {
    if (existsSync(directory)) {
      const stat = lstatSync(directory);
      if (!stat.isDirectory() || stat.isSymbolicLink()) fail('Private backup directories must be regular directories.');
    } else mkdirSync(directory, { mode: 0o700 });
    chmodSync(directory, 0o700);
  }
  const stamp = new Date().toISOString().replace(/[-:.]/g, '');
  const backup = path.join(root, 'private', 'backups', 'access-settings-' + stamp + '-' + randomUUID() + '.env');
  writeFileSync(backup, original, { flag: 'wx', mode: 0o600 });
  temporary = path.join(root, '.env.pilot.local.' + randomUUID() + '.tmp');
  const fd = openSync(temporary, 'wx', 0o600);
  try { writeFileSync(fd, updated, 'utf8'); fsyncSync(fd); } finally { closeSync(fd); }
  if (lstatSync(target).isSymbolicLink() || !readFileSync(target).equals(original)) {
    fail('Settings changed concurrently. Retry after the other editor finishes; no replacement was made.');
  }
  renameSync(temporary, target);
  temporary = undefined;
  console.log(description + '. Secrets and all other settings were preserved.');
  console.log('Private settings backup: ' + path.relative(root, backup));
  console.log(args[0] === 'public' ?
    'Every authenticated MAX user may enter after deployment. Moderator rights remain restricted to assigned MAX IDs.' :
    'After deployment only PILOT_ALLOWED_MAX_IDS may enter. An empty list denies every account, including moderators.');
  console.log('This command did not contact MAX or restart containers. Run: bash scripts/pilot.sh deploy');
} catch (error) {
  console.error(error instanceof AccessError ? error.message :
    'Could not change release settings. Check local file permissions and available disk space; secret values are hidden.');
  process.exitCode = 1;
} finally {
  if (temporary) { try { unlinkSync(temporary); } catch {} }
}
