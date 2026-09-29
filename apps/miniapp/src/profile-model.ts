import type {CompetencyInput,Facet,Profile,ProfileInput,Taxonomy,Topic} from './types';
export function topicPath(topicId:string,topics:Topic[]):Topic[] {
  const path:Topic[]=[];const seen=new Set<string>();let topic=topics.find(item=>item.id===topicId);
  while(topic&&!seen.has(topic.id)){seen.add(topic.id);path.unshift(topic);const parent=topic.parent_id;topic=topics.find(item=>item.id===parent)}
  return path;
}
export function applicableFacets(topicId:string,facets:Facet[]):Facet[] {return facets.filter(facet=>facet.applicable_topic_prefixes.some(prefix=>prefix==='*'||topicId===prefix||topicId.startsWith(`${prefix}.`)))}
export function selectTopic(competency:CompetencyInput,topicId:string,taxonomy:Taxonomy):CompetencyInput {
  const allowed=applicableFacets(topicId,taxonomy.facets);
  return {...competency,topicId,facets:Object.fromEntries(Object.entries(competency.facets).filter(([id])=>allowed.some(facet=>facet.id===id)))};
}
export function profileInput(profile:Profile):ProfileInput {
  return {displayName:profile.displayName,bio:profile.bio,availableToHelp:profile.availableToHelp,maxActiveConversations:profile.maxActiveConversations,competencies:profile.competencies.map(({id,topicId,facets,experienceKind,description,evidenceUrl,evidenceVisibility})=>({...id?{id}:{},topicId,facets,experienceKind,description,...evidenceUrl?{evidenceUrl}:{},evidenceVisibility}))};
}
export function validEvidenceUrl(value:string):boolean {if(!value.trim())return true;try{const url=new URL(value);return url.protocol==='https:'&&!url.username&&!url.password}catch{return false}}
export function newCompetency():CompetencyInput {return {topicId:'',facets:{},experienceKind:'self_study',description:'',evidenceVisibility:'private'}}
