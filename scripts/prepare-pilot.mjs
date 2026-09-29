import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { randomBytes } from 'node:crypto';
import { loadEnvFile } from 'node:process';
import { createInterface } from 'node:readline/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const target=path.join(root,'.env.pilot.local');
const usage='Usage: node scripts/prepare-pilot.mjs https://your-host [--closed] [--allow 123,456]\n       node scripts/prepare-pilot.mjs --prompt\nDefault: every authenticated MAX user. --allow implies --closed.';
class PrepareError extends Error {
 constructor(message,exitCode=1){super(message);this.exitCode=exitCode;}
}
const fail=(message,exitCode)=>{throw new PrepareError(message,exitCode);};
const placeholder=value=>/^(?:REPLACE[_-]|CHANGE[_-]?ME(?:[_-]|$)|YOUR_(?:BOT_|MAX_)?(?:TOKEN|USERNAME)(?:[_-]|$))/i.test(value);

async function promptSettings(username){
 if(!process.stdin.isTTY||!process.stdout.isTTY)
  fail('--prompt requires an interactive terminal. In Docker, add -it; otherwise use the HTTPS URL and optional --allow arguments.');
 const terminal=createInterface({input:process.stdin,output:process.stdout,terminal:true});
 const controller=new AbortController();
 const cancel=()=>controller.abort();
 terminal.once('SIGINT',cancel);
 terminal.once('close',cancel);
 process.once('SIGINT',cancel);
 try {
  const ask=message=>terminal.question(message,{signal:controller.signal});
  console.log('Enter public deployment settings only. The token is read from .env.max.local or the process environment.');
  const origin=(await ask('Public HTTPS origin: ')).trim();
  const botUsername=(await ask(`MAX bot username [${username}]: `)).trim()||username;
  const access=(await ask('Access: public or closed [public]: ')).trim()||'public';
  if(!['public','closed'].includes(access))fail('Access must be public or closed.');
  const closed=access==='closed';
  const allow=closed?(await ask('Allowed MAX IDs, comma separated (empty = nobody): ')).trim():'';
  return {origin,username:botUsername,allow,closed};
 } catch(error) {
  if(controller.signal.aborted)fail('Pilot setup cancelled; no configuration was written.',130);
  throw error;
 } finally {
  terminal.removeListener('close',cancel);
  terminal.close();
  process.removeListener('SIGINT',cancel);
 }
}

try {
 const args=process.argv.slice(2);
 const interactive=args.length===1&&args[0]==='--prompt';
 let closed=false,allow='',allowSeen=false,closedSeen=false;
 if(!interactive){
  if(!args.length||args[0].startsWith('--'))fail(usage);
  for(let i=1;i<args.length;i++){
   if(args[i]==='--closed'&&!closedSeen){closed=true;closedSeen=true;}
   else if(args[i]==='--allow'&&!allowSeen&&i+1<args.length&&!args[i+1].startsWith('--')){
    allow=args[++i];allowSeen=true;closed=true;
   } else fail(usage);
  }
 }
 if(existsSync(target))fail('.env.pilot.local already exists; edit it locally. Existing secrets were not changed.');
 const localEnv=path.join(root,'.env.max.local');
 if(existsSync(localEnv))loadEnvFile(localEnv);
 const token=process.env.MAX_BOT_TOKEN??'';
 if(!token||placeholder(token)||/^x{20,}$/i.test(token)||!/^[A-Za-z0-9_-]{20,4096}$/.test(token))
  fail('Set the real MAX_BOT_TOKEN in the ignored .env.max.local file using a local text editor, then retry. Use MAX_BOT_TOKEN= followed by the token; restrict file permissions (chmod 600 on Linux). Never put the token in command arguments or logs.');
 const defaultUsername=process.env.MAX_BOT_USERNAME||'REPLACE_WITH_BOT_USERNAME';
 if(placeholder(defaultUsername)||!/^[A-Za-z0-9_]{1,128}$/.test(defaultUsername))fail('Invalid MAX_BOT_USERNAME in the local environment.');
 const caPath=path.join(root,'private/max-ca.pem');
 if(!existsSync(caPath))fail('Download the official MAX CA chain into private/max-ca.pem first; see docs/DEPLOYMENT.md.');
 const ca=readFileSync(caPath);
 if(ca.length>128*1024||!ca.toString('ascii').includes('-----BEGIN CERTIFICATE-----'))fail('Invalid CA bundle; see the closed pilot guide.');
 const settings=interactive?await promptSettings(defaultUsername):{origin:args[0],username:defaultUsername,allow,closed};
 let url;
 try {url=new URL(settings.origin);} catch {fail('Use one public HTTPS origin on port 443, without a path or credentials.');}
 if(url.protocol!=='https:'||url.username||url.password||url.port||url.pathname!=='/'||url.search||url.hash||
  !/^(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z]{2,63}$/i.test(url.hostname)||
  /(?:^|\.)(?:local|localhost|test|invalid|example|example\.com|example\.net|example\.org)$/.test(url.hostname)||
  /(?:^|\.)(?:your-domain|your-pilot-host)(?:\.|$)/.test(url.hostname))
  fail('Use your real public HTTPS origin on port 443, without a path, credentials, or an example domain.');
 const allowed=settings.allow===''?[]:settings.allow.split(',').map(value=>value.trim());
 if(allowed.length>500||allowed.some(value=>!/^[1-9][0-9]{0,18}$/.test(value)||BigInt(value)>9223372036854775807n))
  fail('Allowlist must contain at most 500 positive canonical MAX IDs.');
 const username=settings.username;
 if(placeholder(username)||!/^[A-Za-z0-9_]{1,128}$/.test(username))fail('Invalid MAX_BOT_USERNAME.');
 const values={APP_ENV:'production',DEMO_MODE:'false',PILOT_MODE:settings.closed?'true':'false',APP_PUBLIC_URL:url.origin,PILOT_PORT:'8085',
  PILOT_ALLOWED_MAX_IDS:[...new Set(allowed)].join(','),MODERATOR_MAX_IDS:'',POSTGRES_PASSWORD:randomBytes(24).toString('hex'),
  MAX_BOT_TOKEN:token,MAX_BOT_USERNAME:username,MAX_WEBHOOK_SECRET:randomBytes(32).toString('hex'),
  MAX_CA_FILE:'private/max-ca.pem',PILOT_MAX_CA_FILE:'./private/max-ca.pem',
  MAX_API_BASE_URL:'https://platform-api2.max.ru',MAX_DELIVERY_ENABLED:'false',
  MAX_PRODUCT_NOTIFICATIONS_ENABLED:'false'};
 writeFileSync(target,'# Local secrets: never commit or paste into logs.\n'+Object.entries(values).map(([key,value])=>key+'='+value).join('\n')+'\n',{flag:'wx',mode:0o600});
 console.log('Created .env.pilot.local with separate database credentials and '+(settings.closed?'closed pilot access.':'access for all authenticated MAX users.')+' No deployment or MAX subscription was performed.');
 if(settings.closed&&!allowed.length)console.log('The closed allowlist is empty: no account can enter until you add allowed IDs.');
} catch(error) {
 console.error(error instanceof PrepareError?error.message:'Pilot configuration could not be prepared; check the URL, local files, and write permissions. Secret details are hidden.');
 process.exitCode=error instanceof PrepareError?error.exitCode:1;
}
