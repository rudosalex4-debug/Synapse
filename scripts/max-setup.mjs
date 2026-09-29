import { existsSync, readFileSync } from 'node:fs';
import https from 'node:https';
import { loadEnvFile } from 'node:process';

class SetupError extends Error {}
const fail = (message) => { throw new SetupError(message); };
const [mode, ...flags] = process.argv.slice(2);
const usage = 'Usage: node scripts/max-setup.mjs check | commands [--confirm] | subscriptions | subscribe --confirm';
// Only commands handled by apps/backend/src/max_bot.cpp. MAX expects names without '/'.
const commands = [
  { name: 'start', description: 'Начать работу с Синапсом' },
  { name: 'menu', description: 'Открыть главное меню' },
  { name: 'ask', description: 'Задать вопрос об учёбе или карьере' },
  { name: 'knowledge', description: 'Добавить знание, которым могу помочь' },
  { name: 'requests', description: 'Мои вопросы и отклики' },
  { name: 'stats', description: 'Моя статистика' },
  { name: 'resume', description: 'Продолжить незавершённый ввод' },
  { name: 'back', description: 'Вернуться на предыдущий шаг' },
  { name: 'cancel', description: 'Отменить текущий ввод' },
  { name: 'rules', description: 'Правила сообщества' },
  { name: 'help', description: 'Помощь по командам' },
];
const updateTypes = ['bot_started', 'bot_stopped', 'dialog_removed', 'message_created', 'message_callback'];
function apiBase() {
  const url = new URL(process.env.MAX_API_BASE_URL || 'https://platform-api2.max.ru');
  if (url.origin !== 'https://platform-api2.max.ru' || url.pathname !== '/' ||
      url.username || url.password || url.search || url.hash) {
    fail('MAX_API_BASE_URL must be https://platform-api2.max.ru; token is never sent to another host.');
  }
  return url;
}
async function api(path, init = {}) {
  const url = new URL(path, apiBase());
  const ca = process.env.MAX_CA_FILE ? readFileSync(process.env.MAX_CA_FILE) : undefined;
  if (ca && ca.length > 128 * 1024) fail('MAX_CA_FILE is too large.');
  return new Promise((resolve, reject) => {
    const request = https.request(url, {
      method: init.method || 'GET', ca, rejectUnauthorized: true, timeout: 15000,
      headers: { Authorization: process.env.MAX_BOT_TOKEN, 'Content-Type': 'application/json' },
    }, response => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', part => {
        body += part;
        if (body.length > 1024 * 1024) request.destroy(new SetupError('MAX response exceeds size limit.'));
      });
      response.on('end', () => {
        if (response.statusCode < 200 || response.statusCode >= 300) {
          reject(new SetupError('MAX API returned HTTP ' + response.statusCode + '; response body and token are hidden.'));
          return;
        }
        try { resolve(JSON.parse(body)); } catch { reject(new SetupError('MAX response is not valid JSON.')); }
      });
      response.on('error', () => reject(new SetupError('MAX response interrupted; retry later.')));
    });
    request.on('timeout', () => request.destroy(new SetupError('MAX request timed out.')));
    request.on('error', error => reject(error instanceof SetupError ? error :
      new SetupError('MAX connection failed; check connectivity and trusted CA (MAX_CA_FILE). TLS verification remains enabled.')));
    if (init.body) request.write(init.body);
    request.end();
  });
}

function commandStatus(bot) {
  const registered = Array.isArray(bot.commands) ? bot.commands : [];
  const names = registered.map(command => command?.name)
    .filter(name => typeof name === 'string' && /^[a-zA-Z0-9_]{1,64}$/.test(name));
  return {
    registeredCommands: names,
    missingCommands: commands.filter(command => !names.includes(command.name)).map(command => command.name),
    descriptionsToUpdate: commands.filter(command => registered.some(item =>
      item?.name === command.name && item.description !== command.description)).map(command => command.name),
    unexpectedCommandCount: registered.filter(item => !commands.some(command => command.name === item?.name)).length,
  };
}

async function botIdentity() {
  const bot = await api('/me');
  if (bot?.is_bot !== true) fail('The API did not confirm a bot identity.');
  const expected = process.env.MAX_BOT_USERNAME?.trim().replace(/^@/, '').toLowerCase();
  if (expected && (typeof bot.username !== 'string' || bot.username.toLowerCase() !== expected)) {
    fail('MAX_BOT_USERNAME does not match the token owner. Check the selected environment file.');
  }
  return bot;
}

function webhookUrl() {
  const url = new URL(process.env.APP_PUBLIC_URL || '');
  if (url.protocol !== 'https:' || url.port || url.username || url.password ||
      url.search || url.hash || url.pathname !== '/' ||
      !/^(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z]{2,63}$/i.test(url.hostname) ||
      /\.(local|localhost|test|invalid|example)$/.test(url.hostname)) {
    fail('APP_PUBLIC_URL must be a public HTTPS origin on port 443.');
  }
  return new URL('/webhooks/max', url).href;
}

async function main() {
  if (!['check', 'commands', 'subscriptions', 'subscribe'].includes(mode)) fail(usage);
  if (['check', 'subscriptions'].includes(mode) && flags.length) fail(usage);
  if (mode === 'commands' && flags.length && (flags.length !== 1 || flags[0] !== '--confirm')) fail(usage);
  if (mode === 'subscribe' && (flags.length !== 1 || flags[0] !== '--confirm')) {
    fail('Subscription changes a live bot configuration. Add --confirm only after your HTTPS endpoint is deployed.');
  }
  // Preview needs neither credentials nor network access and does not read an env file.
  if (mode === 'commands' && flags.length === 0) {
    console.log(JSON.stringify({ preview: true, commands }, null, 2));
    console.log('Add --confirm to replace the complete command menu on the bot identified by MAX_BOT_TOKEN.');
    return;
  }
  if (existsSync('.env')) loadEnvFile('.env');
  if (process.env.NODE_TLS_REJECT_UNAUTHORIZED === '0') fail('TLS verification must remain enabled.');
  if (!process.env.MAX_BOT_TOKEN?.trim()) fail('MAX_BOT_TOKEN is not set. Local development does not need it.');
  if (mode === 'check') {
    const bot = await botIdentity();
    console.log(JSON.stringify({ ok: true, botId: bot.user_id, username: bot.username ?? null, ...commandStatus(bot) }, null, 2));
    return;
  }
  if (mode === 'commands') {
    await botIdentity();
    await api('/me/commands', { method: 'PATCH', body: JSON.stringify({ commands }) });
    console.log('MAX accepted the command menu update. Reading back /me to confirm the saved list.');
    const status = commandStatus(await botIdentity());
    console.log(JSON.stringify(status, null, 2));
    if (status.missingCommands.length || status.descriptionsToUpdate.length || status.unexpectedCommandCount) {
      fail('The saved menu does not yet match the requested list. Retry check before changing it again.');
    }
    console.log('Command menu confirmed. Reopen the bot chat and type /. This does not configure webhook delivery.');
    return;
  }
  if (mode === 'subscriptions') {
    const expected = webhookUrl();
    await botIdentity();
    const data = await api('/subscriptions');
    if (!Array.isArray(data?.subscriptions)) fail('MAX returned an unexpected subscriptions format; response hidden.');
    const subscriptions = data.subscriptions.map(subscription => ({
      matchesConfiguredEndpoint: subscription?.url === expected,
      // Report only our known update types; never print arbitrary URLs, secrets or API response bodies.
      missingRequiredUpdateTypes: Array.isArray(subscription?.update_types)
        ? updateTypes.filter(type => !subscription.update_types.includes(type)) : null,
      updateTypesExplicit: Array.isArray(subscription?.update_types),
    }));
    console.log(JSON.stringify({ subscriptionCount: subscriptions.length,
      configuredEndpointFound: subscriptions.some(subscription => subscription.matchesConfiguredEndpoint),
      subscriptions }, null, 2));
    console.log('Subscription presence does not prove delivery. Check HTTPS, webhook secret, worker and (only in closed mode) the participant allowlist separately.');
    return;
  }
  if (mode === 'subscribe') {
    if (process.env.APP_ENV !== 'production' || process.env.DEMO_MODE !== 'false') {
      fail('Set APP_ENV=production and DEMO_MODE=false before subscribing a live bot.');
    }
    const url = webhookUrl();
    const secret = process.env.MAX_WEBHOOK_SECRET ?? '';
    if (!/^[a-zA-Z0-9_-]{32,256}$/.test(secret)) fail('Set a random 32–256 character MAX_WEBHOOK_SECRET.');
    await botIdentity();
    const data = await api('/subscriptions', {
      method: 'POST',
      body: JSON.stringify({
        url,
        update_types: updateTypes,
        secret,
      }),
    });
    if (data?.success !== true) fail('MAX did not confirm subscription success; response body hidden.');
    console.log('Webhook subscription registered. Test bot and mini app in MAX web and mobile manually.');
  }
}

try {
  await main();
} catch (error) {
  // Parsing/network failures may contain sensitive source text. Never echo those messages.
  console.error(error instanceof SetupError ? error.message :
    'Request/configuration failed. Check environment, connectivity and trusted TLS certificates; details hidden.');
  process.exitCode = 1;
}
