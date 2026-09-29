// Local recovery test. Stops/restarts this Compose project's database and processes.
// Use a disposable demo database without other users editing it.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const projectRoot=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const base='http://127.0.0.1:8080';
function compose(...args) {
 const common={encoding:'utf8',stdio:['ignore','pipe','pipe'],timeout:45000,windowsHide:true,cwd:projectRoot};
 if(process.platform!=='win32') return execFileSync('docker',['compose',...args],common);
 const normalized=projectRoot.replaceAll('\\','/');
 const match=/^([A-Za-z]):\/(.+)$/.exec(normalized);
 if(!match)throw new Error('Run this recovery test directly inside WSL for a UNC workspace.');
 const linuxRoot='/mnt/'+match[1].toLowerCase()+'/'+match[2];
 return execFileSync('wsl',['-d',process.env.SYNAPSE_WSL_DISTRO || 'kali-linux','--','docker','compose','--project-directory',linuxRoot,...args],common);
}

const req=async(path,{status=200,...options}={})=>{const r=await fetch(base+path,{...options,headers:{'Content-Type':'application/json',...options.headers},signal:AbortSignal.timeout(10000)});assert.equal(r.status,status,path);return r.json()};
const ready=async()=>{for(let n=0;n<30;n++){try{await req('/health/ready');return}catch{}await new Promise(r=>setTimeout(r,500))}throw new Error('DB recovery timeout')};
const session=await req('/api/auth/demo',{method:'POST',body:JSON.stringify({persona:'anna'})});
const headers={Authorization:'Bearer '+session.token};
const rules=await req('/api/community-rules');
const accepted=await req('/api/community-rules/accept',{method:'POST',headers,body:JSON.stringify({version:rules.version})});
assert.equal(accepted.accepted,true);assert.equal(accepted.version,rules.version);
const original=await req('/api/profile',{headers});
const edit=p=>({displayName:p.displayName,bio:p.bio,availableToHelp:p.availableToHelp,maxActiveConversations:p.maxActiveConversations,competencies:p.competencies.map(({id,topicId,facets,experienceKind,description,evidenceVisibility,evidenceUrl})=>({id,topicId,facets,experienceKind,description,evidenceVisibility,...(evidenceUrl?{evidenceUrl}:{})}))});
const saved=await req('/api/profile',{method:'PUT',headers:{...headers,'If-Match':'"'+original.revision+'"'},body:JSON.stringify({...edit(original),bio:'Restart persistence check'})});
try{
 compose('stop','db');
 await req('/health/live');await req('/health/ready',{status:503});await req('/api/profile',{headers,status:503});
 console.log('PASS DB outage: live200, ready503, profile503');
 compose('start','db');await ready();
 assert.deepEqual(await req('/api/profile',{headers}),saved);
 compose('restart','backend','worker');await ready();
 assert.deepEqual(await req('/api/profile',{headers}),saved);
 console.log('PASS database/API/worker restart preserves profile and valid session');
 compose('run','--rm','init');await ready();
 assert.deepEqual(await req('/api/profile',{headers}),saved);
 console.log('PASS repeat migrations/seed preserve edited profile');
}finally{
 compose('start','db');await ready();
 const now=await req('/api/profile',{headers});
 await req('/api/profile',{method:'PUT',headers:{...headers,'If-Match':'"'+now.revision+'"'},body:JSON.stringify(edit(original))});
 await req('/api/auth/logout',{method:'POST',headers});
}
