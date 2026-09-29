import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { execFileSync } from 'node:child_process';
const base = new URL(process.argv[2] || 'http://127.0.0.1:8080');
if (!['localhost','127.0.0.1','[::1]'].includes(base.hostname) || base.username || base.password || base.pathname !== '/' || !['http:','https:'].includes(base.protocol)) throw new Error('Integration checks require a loopback origin.');
let checks=0;
async function request(path,{method='GET',token,body,status=200,headers={}}={}) {
 const r=await fetch(new URL(path,base),{method,headers:{...(body!==undefined?{'Content-Type':'application/json'}:{}),...(token?{Authorization:'Bearer '+token}:{}),...headers},body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(10000),redirect:'error'});
 assert.equal(r.status,status,path+' status');checks++;const data=await r.json();
 if(status>=400){assert.equal(typeof data.error?.code,'string');assert.equal(typeof data.error?.requestId,'string');}
 return {data,r};
}
const edit = p => ({displayName:p.displayName,bio:p.bio,availableToHelp:p.availableToHelp,maxActiveConversations:p.maxActiveConversations,competencies:p.competencies.map(({id,topicId,facets,experienceKind,description,evidenceVisibility,evidenceUrl})=>({id,topicId,facets,experienceKind,description,evidenceVisibility,...(evidenceUrl?{evidenceUrl}:{})}))});
await request('/health/live');await request('/health/ready');
const {data:bootstrap}=await request('/api/bootstrap');assert.equal(bootstrap.mode,'demo');
const {data:catalog}=await request('/api/taxonomy');assert.ok(catalog.topics.filter(t=>t.level===1).length>=5);
await request('/api/profile',{status:401});
await request('/api/auth/demo',{method:'POST',body:{persona:'invented'},status:400});
const {data:a}=await request('/api/auth/demo',{method:'POST',body:{persona:'anna'}});
const {data:b}=await request('/api/auth/demo',{method:'POST',body:{persona:'boris'}});
assert.notEqual(a.user.id,b.user.id);
const {data:oldA,r:oldHeaders}=await request('/api/profile',{token:a.token});
const {data:oldB}=await request('/api/profile',{token:b.token});
assert.equal(oldHeaders.headers.get('etag'),'"'+oldA.revision+'"');
let latest=oldA;
try {
 const body={...edit(oldA),bio:'integration-'+randomUUID(),competencies:[{topicId:'science.math',facets:{},experienceKind:'practice',description:'Объясняю проценты',evidenceVisibility:'private',evidenceUrl:'https://example.invalid/diploma'}]};
 await request('/api/profile',{method:'PUT',token:a.token,body,status:428});
 const saved=await request('/api/profile',{method:'PUT',token:a.token,body,headers:{'If-Match':'"'+oldA.revision+'"'}});
 latest=saved.data;
 assert.equal(latest.revision,oldA.revision+1);assert.equal(latest.competencies[0].evidenceStatus,'unreviewed');
 assert.equal(latest.competencies[0].provenance,'demo');
 await request('/api/profile',{method:'PUT',token:a.token,body,headers:{'If-Match':'"'+oldA.revision+'"'},status:409});
 const {data:persisted}=await request('/api/profile',{token:a.token});assert.deepEqual(persisted,latest);
 const {data:untouched}=await request('/api/profile',{token:b.token});assert.deepEqual(untouched,oldB);
 await request('/api/profile',{method:'PUT',token:b.token,body:{...edit(oldB),competencies:edit(latest).competencies},headers:{'If-Match':'"'+oldB.revision+'"'},status:403});
 for(const invalid of [
  {...edit(latest),provenance:'verified'},
  {...edit(latest),competencies:[{...edit(latest).competencies[0],evidenceStatus:'verified'}]},
  {...edit(latest),competencies:[{...edit(latest).competencies[0],topicId:'science'}]},
  {...edit(latest),competencies:[{...edit(latest).competencies[0],facets:{organization:['itmo']}}]},
  {...edit(latest),competencies:[{...edit(latest).competencies[0],evidenceUrl:'http://example.invalid/file'}]},
  {...edit(latest),bio:'я'.repeat(501)}
 ]) await request('/api/profile',{method:'PUT',token:a.token,body:invalid,headers:{'If-Match':'"'+latest.revision+'"'},status:400});
 const duplicate=await fetch(new URL('/api/auth/demo',base),{method:'POST',headers:{'Content-Type':'application/json'},body:'{"persona":"anna","persona":"boris"}'});assert.equal(duplicate.status,400);checks++;
 await request('/api/profile',{method:'PUT',token:a.token,body:{...edit(latest),competencies:[]},headers:{'If-Match':'"'+latest.revision+'"'},status:200}).then(result=>latest=result.data);
} finally {
 // Restore the original demo fields, preserving assigned competency IDs where possible.
 // Deleted demo competencies are recreated; a competing user's save is never overwritten.
 const restore=edit(oldA);restore.competencies=restore.competencies.map(({id,...skill})=>skill);
 const restored=await request('/api/profile',{method:'PUT',token:a.token,body:restore,headers:{'If-Match':'"'+latest.revision+'"'}});
 latest=restored.data;
}
await request('/api/auth/logout',{method:'POST',token:a.token});
await request('/api/profile',{token:a.token,status:401});
await request('/api/auth/logout',{method:'POST',token:b.token});
await request('/webhooks/max',{method:'POST',body:{update_type:'unknown'},status:403});
await request('/webhooks/max',{method:'POST',body:{update_type:'unknown'},headers:{'X-Max-Bot-Api-Secret':process.env.MAX_WEBHOOK_SECRET || 'local-webhook-test-only'}});
if(!bootstrap.maxConfigured) await request('/api/auth/max',{method:'POST',body:{initData:'auth_date=1&user=x&hash=bad'},status:503});
console.log('PASS '+checks+' HTTP integration scenarios: persistence, isolation, validation, revision conflicts, logout, webhook secret.');
console.log('These checks change and restore the two local demo profiles; run on a disposable development database.');
