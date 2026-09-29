import {describe,expect,it} from 'vitest';
import {bridgeSummary} from './bridge';
import {ApiError,errorMessage} from './api';
import {applicableFacets,profileInput,selectTopic,topicPath,validEvidenceUrl} from './profile-model';
import type {Profile,Taxonomy} from './types';
const catalog:Taxonomy={version:'test',topics:[{id:'digital',parent_id:null,level:1,label:'Цифровые',active:true},{id:'digital.sheets',parent_id:'digital',level:2,label:'Таблицы',active:true},{id:'digital.sheets.pivots',parent_id:'digital.sheets',level:3,label:'Сводные',active:true},{id:'languages',parent_id:null,level:1,label:'Языки',active:true},{id:'languages.english',parent_id:'languages',level:2,label:'Английский',active:true}],facets:[{id:'language',label:'Язык',applicable_topic_prefixes:['*'],values:[{id:'ru',label:'Русский'}]},{id:'tool',label:'Инструмент',applicable_topic_prefixes:['digital.sheets'],values:[{id:'excel',label:'Excel'}]}]};
describe('границы тем профиля',()=>{
  it('удаляет контекст другой области при смене темы и сохраняет общий',()=>{expect(selectTopic({topicId:'digital.sheets.pivots',facets:{language:['ru'],tool:['excel']},experienceKind:'practice',description:'',evidenceVisibility:'private'},'languages.english',catalog).facets).toEqual({language:['ru']})});
  it('не смешивает префиксы разных тем с похожими именами',()=>{expect(applicableFacets('digital.sheetsOther',catalog.facets).map(item=>item.id)).toEqual(['language'])});
  it('строит уровни по родителям, не по отображаемому тексту',()=>{expect(topicPath('digital.sheets.pivots',catalog.topics).map(item=>item.level)).toEqual([1,2,3])});
  it('останавливает поврежденный циклический каталог',()=>{expect(topicPath('x',[{id:'x',parent_id:'y',level:2,label:'x',active:true},{id:'y',parent_id:'x',level:1,label:'y',active:true}])).toHaveLength(2)});
});
describe('границы доверия',()=>{
  it('не передает серверные поля доверия в сохранение профиля',()=>{const profile={id:'user',revision:7,displayName:'Анна',bio:'',availableToHelp:true,maxActiveConversations:2,provenance:'demo',competencies:[{id:'skill',topicId:'languages.english',facets:{},experienceKind:'practice',description:'',evidenceVisibility:'private',evidenceStatus:'unreviewed',provenance:'demo'}]} as Profile;const body=profileInput(profile);expect(body).not.toHaveProperty('provenance');expect(body).not.toHaveProperty('revision');expect(body.competencies[0]).not.toHaveProperty('provenance');expect(body.competencies[0]).not.toHaveProperty('evidenceStatus');expect(body.competencies[0].id).toBe('skill')});
  it.each(['javascript:alert(1)','http://example.org','https://user:secret@example.org','not-a-url'])('отклоняет небезопасную ссылку %s',value=>expect(validEvidenceUrl(value)).toBe(false));
  it('принимает HTTPS-ссылку и отсутствие подтверждения',()=>{expect(validEvidenceUrl('https://example.org/portfolio')).toBe(true);expect(validEvidenceUrl('')).toBe(true)});
  it('диагностика не содержит подпись запуска или произвольный platform',()=>{const data=bridgeSummary({initData:'sensitive-hash',platform:'user@example.org'},'loaded');expect(JSON.stringify(data)).not.toContain('sensitive-hash');expect(JSON.stringify(data)).not.toContain('user@example.org');expect(data.hasLaunchData).toBe(true)});
  it('наличие библиотеки не выдает браузер за подтвержденный вход',()=>{expect(bridgeSummary({},'loaded').hasLaunchData).toBe(false)});
  it('ошибка API не раскрывает сообщение сервера с секретом',()=>{expect(errorMessage(new ApiError(500,'sensitive-server-data'))).not.toContain('sensitive-server-data')});
});
