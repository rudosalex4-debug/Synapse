import { describe, expect, it } from 'vitest';
import type { CreateRequest, HelpRequest, Message } from './workflow-api';
import { changeRequestFacet, changeRequestTopic, mergeMessages, newRequest, requestInput, requestValidation, shouldPoll } from './workflow-model';
import type { Taxonomy } from './types';

const taxonomy: Taxonomy = {
  version: 'test-v1',
  topics: [
    { id: 'digital', parent_id: null, level: 1, label: 'Цифровые знания', active: true },
    { id: 'digital.sheets', parent_id: 'digital', level: 2, label: 'Таблицы', active: true },
    { id: 'digital.sheets.pivots', parent_id: 'digital.sheets', level: 3, label: 'Сводные таблицы', active: true },
    { id: 'languages', parent_id: null, level: 1, label: 'Языки', active: true },
    { id: 'languages.english', parent_id: 'languages', level: 2, label: 'Английский', active: true },
    { id: 'languages.archived', parent_id: 'languages', level: 2, label: 'Архив', active: false },
  ],
  facets: [
    { id: 'language', label: 'Язык общения', applicable_topic_prefixes: ['*'], values: [{ id: 'ru', label: 'Русский' }] },
    { id: 'tool', label: 'Инструмент', applicable_topic_prefixes: ['digital.sheets'], values: [{ id: 'excel', label: 'Excel' }] },
  ],
};
const validDraft = (): CreateRequest => ({
  ...newRequest(taxonomy.version), title: 'Как устроены сводные таблицы?',
  body: 'Хочу понять, как объединить несколько строк по одному признаку.',
  topicId: 'digital.sheets.pivots', facets: { language: ['ru'], tool: ['excel'] }, requiredFacets: ['language', 'tool'],
});
const message = (id: string, sequence: number, text = id): Message => ({
  id, sequence, text, conversationId: 'conversation', senderId: 'person', senderName: 'Участник',
  createdAt: '2026-09-18T12:00:00Z', clientMessageId: `client-${id}`,
});

describe('редактор учебного вопроса', () => {
  it('при смене области удаляет неприменимые обязательные условия, сохраняя общий контекст', () => {
    const draft = validDraft();
    const changed = changeRequestTopic(draft, 'languages.english', taxonomy);
    expect(changed.facets).toEqual({ language: ['ru'] });
    expect(changed.requiredFacets).toEqual(['language']);
    expect(draft.facets).toEqual({ language: ['ru'], tool: ['excel'] });
    expect(draft.requiredFacets).toEqual(['language', 'tool']);
  });
  it('снятие последнего значения снимает обязательность условия и не меняет исходный черновик', () => {
    const draft = validDraft();
    const changed = changeRequestFacet(draft, 'tool', 'excel', false);
    expect(changed.facets).not.toHaveProperty('tool');
    expect(changed.requiredFacets).toEqual(['language']);
    expect(draft.facets.tool).toEqual(['excel']);
  });
  it('повторное включение значения не размножает уточнения', () => {
    expect(changeRequestFacet(validDraft(), 'tool', 'excel', true).facets.tool).toEqual(['excel']);
  });
  it.each(['digital', 'languages.archived', 'unknown'])('не разрешает публикацию без активной темы L2+ (%s)', topicId => {
    expect(requestValidation({ ...validDraft(), topicId }, taxonomy)).not.toBe('');
  });
  it.each(['digital.sheets', 'digital.sheets.pivots'])('разрешает публикацию с активной темой L2+ (%s)', topicId => {
    expect(requestValidation({ ...validDraft(), topicId }, taxonomy)).toBe('');
  });
  it('не считает пробелы подробным описанием и отвергает пустое обязательное условие', () => {
    expect(requestValidation({ ...validDraft(), body: ' '.repeat(30) }, taxonomy)).toContain('30');
    expect(requestValidation({ ...validDraft(), facets: { tool: ['excel'] } }, taxonomy)).toContain('обязательного');
  });
  it('считает символы вне BMP как один символ при проверке предельной длины', () => {
    expect(requestValidation({ ...validDraft(), title: '💡'.repeat(120), body: '💡'.repeat(30) }, taxonomy)).toBe('');
    expect(requestValidation({ ...validDraft(), title: '💡'.repeat(121) }, taxonomy)).toContain('120');
    expect(requestValidation({ ...validDraft(), body: '💡'.repeat(2001) }, taxonomy)).toContain('2000');
  });
  it('редактирование не передаёт владение, состояние, подбор или версию записи в JSON запроса', () => {
    const request: HelpRequest = {
      ...validDraft(), id: 'request', authorId: 'author', authorName: 'Анна', status: 'draft', revision: 7,
      createdAt: '', updatedAt: '', attempt: null, desiredExperience: null,
      match: { score: 80, competencyId: 'skill', topicId: 'digital.sheets', narrower: false },
      conversationId: 'conversation', myOffer: null,
    };
    expect(requestInput(request)).toEqual({ ...validDraft(), attempt: '' });
  });
});

describe('переписка при повторных ответах и смене экрана', () => {
  it('объединяет подтверждение отправки и пересекающуюся страницу без дублей и потери более раннего сообщения', () => {
    const existing = [message('third', 3), message('first', 1)];
    const incoming = [message('second', 2), message('third', 3, 'Подтверждённый текст')];
    const merged = mergeMessages(existing, incoming);
    expect(merged.map(item => item.sequence)).toEqual([1, 2, 3]);
    expect(merged[2].text).toBe('Подтверждённый текст');
    expect(existing.map(item => item.sequence)).toEqual([3, 1]);
  });
  it('сохраняет отдельные сообщения с одинаковым текстом', () => {
    expect(mergeMessages([message('one', 1, 'Спасибо')], [message('two', 2, 'Спасибо')])).toHaveLength(2);
  });
  it.each([
    ['mine', 'chat', false, false], ['feed', 'chat', false, false],
    ['conversations', null, false, false], ['conversations', 'chat', true, false],
    ['conversations', 'chat', false, true],
  ] as const)('останавливает опрос вне активного доступного диалога (%s, %s, %s, %s)', (tab, id, hidden, expired) => {
    expect(shouldPoll(tab, id, hidden, expired)).toBe(false);
  });
  it('разрешает опрос открытого диалога после возвращения на экран', () => {
    expect(shouldPoll('conversations', 'chat', false, false)).toBe(true);
  });
});
