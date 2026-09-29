// Disposable local API scenarios: uses only compose.auth-test.yaml, synthetic MAX identities.
import assert from 'node:assert/strict';
import { createHmac, randomBytes, randomUUID } from 'node:crypto';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const execute=promisify(execFile), root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const project='synapse-auth-test', base='http://127.0.0.1:8082';
let checks=0, started=false;
function check(ok,message){assert.ok(ok,message);checks++;}
async function compose(args,timeout=60000){
 let bin='docker', prefix=['compose','-p',project,'-f',path.join(root,'compose.auth-test.yaml')];
 if(process.platform==='win32'){
  const normalized=root.replaceAll('\\','/'), match=/^([a-zA-Z]):\/(.+)$/.exec(normalized);
  if(!match)throw Error('Use WSL for UNC paths');
  const linux='/mnt/'+match[1].toLowerCase()+'/'+match[2];
  bin='wsl';prefix=['-d',process.env.SYNAPSE_WSL_DISTRO||'kali-linux','--','env','COMPOSE_DISABLE_ENV_FILE=1','SYNAPSE_TEST_IMAGE='+(process.env.SYNAPSE_TEST_IMAGE||'synapse:local'),'docker','compose','-p',project,'--project-directory',linux,'-f',linux+'/compose.auth-test.yaml'];
 }
 try{return await execute(bin,[...prefix,...args],{cwd:root,env:{...process.env,COMPOSE_DISABLE_ENV_FILE:'1'},encoding:'utf8',windowsHide:true,timeout,maxBuffer:2*1024*1024});}
 catch {throw Error('Test Compose operation failed: '+args[0]);}
}
async function api(actor,endpoint,{method='GET',body,status=200,key,revision,headers={}}={}){
 const r=await fetch(base+endpoint,{method,headers:{...(actor?{Authorization:'Bearer '+actor.token}:{}),...(body!==undefined?{'Content-Type':'application/json'}:{}),...(key?{'Idempotency-Key':key}:{}),...(revision!==undefined?{'If-Match':'"'+revision+'"'}:{}),...headers},body:body===undefined?undefined:JSON.stringify(body),redirect:'error',signal:AbortSignal.timeout(12000)});
 const data=await r.json();
 check((Array.isArray(status)?status:[status]).includes(r.status),method+' '+endpoint+' expected '+status+' got '+r.status+' '+(data.error?.code??''));
 if(r.status>=400){check(typeof data.error?.requestId==='string','Structured request ID on error');}
 return data;
}
async function actor(name,skills=[]){
 const params={auth_date:String(Math.floor(Date.now()/1000)),user:JSON.stringify({id:Number.parseInt(randomBytes(6).toString('hex'),16),first_name:name})};
 const launch=Object.entries(params).sort(([a],[b])=>a.localeCompare(b)).map(([k,v])=>k+'='+v).join('\n');
 const secret=createHmac('sha256','WebAppData').update('local-test-token-not-a-real-bot-token').digest();
 const raw=Object.entries(params).map(([k,v])=>k+'='+encodeURIComponent(v)).join('&')+'&hash='+createHmac('sha256',secret).update(launch).digest('hex');
 const session=await api(null,'/api/auth/max',{method:'POST',body:{initData:raw}});
 const accepted=await api(session,'/api/community-rules/accept',{method:'POST',body:{version:rulesVersion}});
 check(accepted.accepted===true&&accepted.version===rulesVersion,'Synthetic actor explicitly accepts current community rules');
 const p=await api(session,'/api/profile');
 session.profile=await api(session,'/api/profile',{method:'PUT',revision:p.revision,body:{displayName:name,bio:'Synthetic workflow acceptance',availableToHelp:true,maxActiveConversations:2,competencies:skills}});
 return session;
}
const skill=(topicId,facets={})=>({topicId,facets,experienceKind:'practice',description:'Объясняю тему на учебных примерах',evidenceVisibility:'private',evidenceUrl:'https://example.invalid/private-proof'});
const editProfile=p=>({displayName:p.displayName,bio:p.bio,availableToHelp:p.availableToHelp,maxActiveConversations:p.maxActiveConversations,competencies:p.competencies.map(({id,topicId,facets,experienceKind,description,evidenceVisibility,evidenceUrl})=>({id,topicId,facets,experienceKind,description,evidenceVisibility,...(evidenceUrl?{evidenceUrl}:{})}))});
const post=(who,url,body={},options={})=>api(who,url,{method:'POST',body,key:randomUUID(),...options});
let version,rulesVersion;
const question=(extra={})=>({title:'Как организовать обучение на первом курсе',body:'Начал вводный онлайн-курс и хочу понять, как распределить занятия и практику по неделе. Поделитесь своим опытом самостоятельного обучения.',learningGoal:'understand',attempt:'Изучил программу курса и составил расписание занятий.',topicId:'pathways.courses.learning',facets:{learner_level:['beginner'],response_language:['ru']},requiredFacets:['learner_level'],taxonomyVersion:version,...extra});
async function published(who,extra={}){const r=await post(who,'/api/requests',question(extra),{status:201});return post(who,'/api/requests/'+r.id+'/publish');}
try{
 const config=JSON.parse((await compose(['config','--format','json'])).stdout);
 check(config.name===project && !config.services.worker && config.services.api.environment.MAX_BOT_TOKEN==='local-test-token-not-a-real-bot-token','Synthetic isolated stack only');
 started=true;await compose(['up','--no-build','--wait','--wait-timeout','120'],150000);
 version=(await api(null,'/api/taxonomy')).version;
 rulesVersion=(await api(null,'/api/community-rules')).version;
 const learning=skill('pathways.courses.learning',{learner_level:['beginner'],response_language:['ru']});
 const a=await actor('Автор',[skill('career.application.interview'),learning]);
 const b=await actor('Помощник Б',[learning]);
 const c=await actor('Помощник В',[learning]);
 const outsider=await actor('Посторонний',[skill('pathways.hackathons.team')]);
 const split=await actor('Раздельные знания',[skill('pathways.courses',{learner_level:['beginner']}),skill('pathways.courses.learning',{learner_level:['basics'],response_language:['ru']})]);
 await api(null,'/api/requests',{status:401});
 await api(a,'/api/requests?unknown=true',{status:400});
 await api(a,'/api/requests?limit=',{status:400});
 await post(a,'/api/requests',question({authorId:outsider.user.id}),{status:400});
 await post(a,'/api/requests',question({topicId:'pathways'}),{status:400});
 await post(a,'/api/requests',question({facets:{organization:['itmo']},requiredFacets:['organization']}),{status:400});
 const key=randomUUID(), body=question();
 const draft=await post(a,'/api/requests',body,{key,status:201});
 const retry=await post(a,'/api/requests',body,{key,status:201});check(retry.id===draft.id,'Create retry does not duplicate request');
 await post(a,'/api/requests',question({title:'Другой вопрос'}),{key,status:409});
 await api(outsider,'/api/requests/'+draft.id,{status:404});
 await api(a,'/api/requests/'+draft.id,{method:'PUT',body,revision:draft.revision+1,status:409});
 const changed=await api(a,'/api/requests/'+draft.id,{method:'PUT',body:question({attempt:'Разобрал первый шаг.'}),revision:draft.revision});
 const opened=await post(a,'/api/requests/'+draft.id+'/publish');check(opened.status==='open','Publish opens request');
 const ownFeed=await api(a,'/api/requests?scope=feed');check(!ownFeed.items.some(r=>r.id===draft.id),'Author excluded even when their competencies fit');
 const feed=await api(b,'/api/requests?scope=feed');const matched=feed.items.find(r=>r.id===draft.id);
 check(matched?.match?.score>=70,'Matching request appears in suitable helper feed with explanation');
 check(!JSON.stringify(matched).includes('private-proof'),'Feed contains no private evidence URL');
 const splitFeed=await api(split,'/api/requests?scope=feed');check(!splitFeed.items.some(r=>r.id===draft.id),'No cross-competency combination of topic and required facets');
 await api(outsider,'/api/requests/'+draft.id,{status:404});
 await post(outsider,'/api/requests/'+draft.id+'/offers',{message:'Неподходящий отклик'},{status:[403,404]});
 const offerB=await post(b,'/api/requests/'+draft.id+'/offers',{message:'Поделюсь опытом обучения на курсе.'},{status:201});
 const offerC=await post(c,'/api/requests/'+draft.id+'/offers',{message:'Расскажу, как планировал занятия и практику.'},{status:201});
 const offers=await api(a,'/api/requests/'+draft.id+'/offers');check(offers.items.length===2,'Author sees both voluntary offers');
 check(!JSON.stringify(offers).includes('private-proof'),'Offer snapshots do not leak private proof');
 const ownOffers=await api(b,'/api/requests/'+draft.id+'/offers');check(ownOffers.items.every(o=>o.helperId===b.user.id),'Helper cannot inspect another helper offer');
 await post(outsider,'/api/offers/'+offerB.id+'/accept',{}, {status:[403,404]});
 // Race author choices: one conversation, never two accepted offers.
 const choices=await Promise.all([
  post(a,'/api/offers/'+offerB.id+'/accept',{}, {status:[200,409]}),
  post(a,'/api/offers/'+offerC.id+'/accept',{}, {status:[200,409]})
 ]);
 const accepted=choices.filter(x=>x.id&&x.helperId);check(accepted.length===1,'Exactly one concurrent choice succeeds');
 const chat=accepted[0], helper=chat.helperId===b.user.id?b:c, other=helper===b?c:b;
 const finalOffers=await api(a,'/api/requests/'+draft.id+'/offers');
 check(finalOffers.items.filter(o=>o.status==='accepted').length===1,'Only one persisted accepted offer');
 await api(other,'/api/conversations/'+chat.id+'/messages',{status:404});
 const clientMessageId=randomUUID(), text='<script>alert("plain text")</script> Учебный вопрос';
 const sent=await post(a,'/api/conversations/'+chat.id+'/messages',{clientMessageId,text},{status:201});
 const resent=await post(a,'/api/conversations/'+chat.id+'/messages',{clientMessageId,text},{status:201});
 check(sent.id===resent.id&&sent.sequence===resent.sequence,'Message retry keeps identity and order');
 await post(a,'/api/conversations/'+chat.id+'/messages',{clientMessageId,text:'Другое содержимое'},{status:409});
 await post(helper,'/api/conversations/'+chat.id+'/messages',{clientMessageId:randomUUID(),text:'Я выделял два вечера на занятия и один на самостоятельную практику.'},{status:201});
 const history=await api(a,'/api/conversations/'+chat.id+'/messages?after=0&limit=1');
 check(history.items.length===1 && history.items[0].text===text,'Plain text stored unchanged with pagination');
 const rest=await api(helper,'/api/conversations/'+chat.id+'/messages?after='+history.nextAfter+'&limit=50');
 check(rest.items.length===1&&rest.items[0].sequence>history.items[0].sequence,'Message sequence cursor works');
 const report=await post(helper,'/api/reports',{conversationId:chat.id,category:'other',text:'Синтетическая проверка права участника.'},{status:201});check(report.status==='new','Participant can report');
 await post(outsider,'/api/reports',{conversationId:chat.id,category:'other',text:'Недоступный разговор'},{status:404});
 await post(helper,'/api/conversations/'+chat.id+'/close',{outcome:'helpful'},{status:403});
 const closeKey=randomUUID();
 const closed=await post(a,'/api/conversations/'+chat.id+'/close',{outcome:'helpful',comment:'Составил план занятий и практики.'},{key:closeKey});
 check(closed.status==='closed'&&closed.outcome==='helpful','Author records outcome and closes');
 await post(a,'/api/conversations/'+chat.id+'/close',{outcome:'helpful',comment:'Составил план занятий и практики.'},{key:closeKey});
 await post(helper,'/api/conversations/'+chat.id+'/messages',{clientMessageId:randomUUID(),text:'Позднее сообщение'},{status:409});
 check((await api(a,'/api/requests/'+draft.id)).status==='resolved','Helpful result resolves request');

 // Same people switch roles across domains.
 const interview=await published(helper,{topicId:'career.application.interview',facets:{},requiredFacets:[],title:'Как подготовиться к первому собеседованию',body:'Хочу узнать, как вы готовились к первому собеседованию и выбирали примеры своих работ. Поделитесь личным опытом подготовки.',attempt:'Прочитал описание вакансии и выписал свои учебные проекты.'});
 const reverseFeed=await api(a,'/api/requests?scope=feed');check(reverseFeed.items.some(r=>r.id===interview.id),'Former author helps former helper in another domain');
 const reverseOffer=await post(a,'/api/requests/'+interview.id+'/offers',{message:'Теперь я могу поделиться опытом собеседований.'},{status:201});
 const reverseChat=await post(helper,'/api/offers/'+reverseOffer.id+'/accept');
 await post(a,'/api/blocks',{userId:helper.user.id});
 await post(helper,'/api/conversations/'+reverseChat.id+'/messages',{clientMessageId:randomUUID(),text:'Заблокировано'},{status:[403,409]});
 const afterBlock=await api(a,'/api/conversations');check(afterBlock.items.find(x=>x.id===reverseChat.id)?.status==='closed','Block closes existing pair conversation');
 const newBlocked=await published(helper,{topicId:'career.application.interview',facets:{},requiredFacets:[],title:'Как рассказать о проектах на собеседовании',body:'Хочу подготовить рассказ об учебных проектах для собеседования. Поделитесь опытом выбора примеров и структуры ответа.',attempt:'Составил список проектов и своих задач в каждом.'});
 const blockedFeed=await api(a,'/api/requests?scope=feed');check(!blockedFeed.items.some(r=>r.id===newBlocked.id),'Block prevents later matching in either direction');
 
 // Independent owner exercises concurrent max-open limit.
 const limitOwner=await actor('Лимит',[skill('pathways.research.writing')]);
 const drafts=[];for(let i=0;i<4;i++)drafts.push(await post(limitOwner,'/api/requests',question({title:'Проверка лимита '+i}),{status:201}));
 const publications=await Promise.all(drafts.map(r=>post(limitOwner,'/api/requests/'+r.id+'/publish',{}, {status:[200,409]})));
 check(publications.filter(r=>r.status==='open').length===3,'Concurrent publication cannot exceed three open requests');
 const pagination=await api(limitOwner,'/api/requests?scope=mine&limit=2');check(pagination.items.length===2&&pagination.nextCursor,'Request keyset first page');
 const page2=await api(limitOwner,'/api/requests?scope=mine&limit=2&after='+encodeURIComponent(pagination.nextCursor));
 check(page2.items.length===2&&!page2.items.some(r=>pagination.items.some(p=>p.id===r.id)),'Request keyset has no duplicate rows');
 // Current availability and competencies are rechecked at acceptance, not only at offer time.
 const x=await actor('Автор лимита помощи',[learning]);
 const y=await actor('Второй автор лимита помощи',[learning]);
 const shared=await actor('Общий помощник',[learning]);
 let sharedProfile=shared.profile;
 const setShared=async changes=>{sharedProfile=await api(shared,'/api/profile',{method:'PUT',revision:sharedProfile.revision,body:{...editProfile(sharedProfile),...changes}});};
 await setShared({maxActiveConversations:1});
 const rx=await published(x), ry=await published(y);
 const ox=await post(shared,'/api/requests/'+rx.id+'/offers',{message:'Первый отклик'},{status:201});
 const oy=await post(shared,'/api/requests/'+ry.id+'/offers',{message:'Второй отклик'},{status:201});
 await setShared({availableToHelp:false});
 await post(x,'/api/offers/'+ox.id+'/accept',{}, {status:409});
 check((await api(shared,'/api/requests?scope=feed')).items.length===0,'Paused helper has empty feed');
 await setShared({availableToHelp:true,competencies:[skill('career.application.interview')]});
 await post(x,'/api/offers/'+ox.id+'/accept',{}, {status:409});
 await setShared({competencies:[learning]});
 const capacityChoices=await Promise.all([post(x,'/api/offers/'+ox.id+'/accept',{}, {status:[200,409]}),post(y,'/api/offers/'+oy.id+'/accept',{}, {status:[200,409]})]);
 check(capacityChoices.filter(v=>v.status==='active').length===1,'Two authors cannot overbook helper capacity1');
 const activeCapacity=capacityChoices.find(v=>v.status==='active');
 check((await api(shared,'/api/requests?scope=feed')).items.length===0,'Full helper has empty feed');
 await post(shared,'/api/conversations/'+activeCapacity.id+'/close',{outcome:'no_result'});
 const pendingOwner=activeCapacity.authorId===x.user.id?y:x;
 const pendingOffer=pendingOwner===x?ox:oy;
 const reaccepted=await post(pendingOwner,'/api/offers/'+pendingOffer.id+'/accept');
 check(reaccepted.status==='active','Closing chat frees helper capacity');
 await post(shared,'/api/conversations/'+reaccepted.id+'/close',{outcome:'no_result'});
 const withdrawnQuestion=await published(x);
 const withdrawnOffer=await post(shared,'/api/requests/'+withdrawnQuestion.id+'/offers',{message:'Отклик для отзыва'},{status:201});
 const withdrawKey=randomUUID();
 check((await post(shared,'/api/offers/'+withdrawnOffer.id+'/withdraw',{}, {key:withdrawKey})).status==='withdrawn','Helper can withdraw pending offer');
 await post(shared,'/api/offers/'+withdrawnOffer.id+'/withdraw',{}, {key:withdrawKey});
 await post(x,'/api/offers/'+withdrawnOffer.id+'/accept',{}, {status:409});
 await post(shared,'/api/requests/'+withdrawnQuestion.id+'/offers',{message:'Повторный отклик'},{status:409});
 check((await post(x,'/api/requests/'+withdrawnQuestion.id+'/cancel')).status==='cancelled','Author can cancel open question');
 await post(shared,'/api/requests/'+withdrawnQuestion.id+'/offers',{message:'После отмены'},{status:409});
 await post(x,'/api/requests',question({body:' '.repeat(30)}),{status:400});
 await post(x,'/api/requests',question({title:'\u0000'}),{status:400});
 console.log('PASS '+checks+' workflow assertions: rules acceptance/draft/publish/indexed feed/offers/race/chat/retry/outcome/role switch/block/privacy/open limit/capacity/profile changes/withdraw/cancel/query validation.');
} finally {
 if(started)await compose(['down','--timeout','10']);
}
