import type { Taxonomy } from './types';
import { applicableFacets } from './profile-model';
import type { CreateRequest, HelpRequest, Message, Outcome, RequestStatus } from './workflow-api';

export const goalLabels = { understand: 'Понять тему', practice: 'Разобраться на практике', troubleshoot: 'Найти причину ошибки', learning_path: 'Выбрать путь обучения' } as const;
export const experienceLabels = { self_study: 'Самостоятельное изучение', practice: 'Практический опыт', teaching: 'Объяснение и обучение', participation: 'Участие в проектах' } as const;
export const statusLabels: Record<RequestStatus, string> = { draft: 'Черновик', open: 'Ждёт помощи', in_progress: 'Идёт общение', resolved: 'Помощь получена', cancelled: 'Отменён', expired: 'Срок истёк', closed_unresolved: 'Завершён без решения' };
export const outcomeLabels: Record<Outcome, string> = { helpful: 'Разобрался, спасибо', partly_helpful: 'Стало понятнее частично', not_helpful: 'Разобраться не получилось', no_result: 'Завершить без результата' };
export function newRequest(taxonomyVersion: string): CreateRequest {
  return { title: '', body: '', learningGoal: 'understand', attempt: '', topicId: '', facets: {}, requiredFacets: [], taxonomyVersion };
}
export function requestInput(request: HelpRequest): CreateRequest {
  return { title: request.title, body: request.body, learningGoal: request.learningGoal, attempt: request.attempt ?? '', topicId: request.topicId, facets: request.facets, requiredFacets: request.requiredFacets, ...(request.desiredExperience ? { desiredExperience: request.desiredExperience } : {}), taxonomyVersion: request.taxonomyVersion };
}
export function changeRequestTopic(draft: CreateRequest, topicId: string, taxonomy: Taxonomy): CreateRequest {
  const allowed = new Set(applicableFacets(topicId, taxonomy.facets).map(facet => facet.id));
  const facets = Object.fromEntries(Object.entries(draft.facets).filter(([id]) => allowed.has(id)));
  return { ...draft, topicId, taxonomyVersion: taxonomy.version, facets, requiredFacets: draft.requiredFacets.filter(id => allowed.has(id) && facets[id]?.length) };
}
export function changeRequestFacet(draft: CreateRequest, facetId: string, valueId: string, checked: boolean): CreateRequest {
  const values = checked ? [...new Set([...(draft.facets[facetId] ?? []), valueId])] : (draft.facets[facetId] ?? []).filter(id => id !== valueId);
  const facets = { ...draft.facets };
  if (values.length) facets[facetId] = values; else delete facets[facetId];
  return { ...draft, facets, requiredFacets: draft.requiredFacets.filter(id => id !== facetId || values.length > 0) };
}
export function requestValidation(draft: CreateRequest, taxonomy: Taxonomy): string {
  const topic = taxonomy.topics.find(item => item.id === draft.topicId && item.active);
  if (!topic || topic.level < 2) return 'Выберите образовательное или карьерное направление и конкретную тему.';
  if ([...draft.title.trim()].length < 1 || [...draft.title].length > 120) return 'Кратко назовите вопрос: от 1 до 120 символов.';
  const length = [...draft.body.trim()].length;
  if (length < 30 || length > 2000) return 'Опишите вопрос подробнее: от 30 до 2000 символов.';
  if (draft.requiredFacets.some(id => !draft.facets[id]?.length)) return 'Выберите значение для каждого обязательного уточнения.';
  return '';
}
export function mergeMessages(existing: Message[], incoming: Message[]): Message[] {
  const byId = new Map(existing.map(message => [message.id, message]));
  for (const message of incoming) byId.set(message.id, message);
  return [...byId.values()].sort((a, b) => a.sequence - b.sequence);
}
export function shouldPoll(tab: string, conversationId: string | null, documentHidden: boolean, sessionExpired: boolean): boolean {
  return tab === 'conversations' && conversationId !== null && !documentHidden && !sessionExpired;
}
