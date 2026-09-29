export interface Topic {id:string;parent_id:string|null;level:number;label:string;active:boolean;aliases?:string[];description?:string}
export interface Facet {id:string;label:string;applicable_topic_prefixes:string[];values:{id:string;label:string}[];help?:string;placeholder?:string}
export interface Taxonomy {version:string;topics:Topic[];facets:Facet[]}
export interface Bootstrap {mode:'demo'|'max';maxConfigured:boolean;version:string;pilotMode?:boolean}
export interface Session {token:string;expiresAt:string;user:{id:string;displayName:string;provenance:string}}
export type ExperienceKind='self_study'|'practice'|'teaching'|'participation';
export interface CompetencyInput {id?:string;topicId:string;facets:Record<string,string[]>;experienceKind:ExperienceKind;description:string;evidenceUrl?:string;evidenceVisibility:'private'|'participants'}
export interface Competency extends CompetencyInput {evidenceStatus:'none'|'unreviewed';provenance:string}
export interface Profile {id:string;revision:number;displayName:string;bio:string;availableToHelp:boolean;maxActiveConversations:number;provenance:string;competencies:Competency[];archivedCompetencies?:Competency[]}
export interface ProfileInput {displayName:string;bio:string;availableToHelp:boolean;maxActiveConversations:number;competencies:CompetencyInput[]}
