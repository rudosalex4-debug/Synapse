import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from './api';
import { MutationKeys, workflowError, workflowRead, workflowWrite } from './workflow-api';

const fetchMock = vi.fn<typeof fetch>();
function response(status: number, data: unknown): Response {
  return { status, ok: status >= 200 && status < 300, json: async () => data } as Response;
}
beforeEach(() => {
  fetchMock.mockReset();
  vi.stubGlobal('window', { setTimeout, clearTimeout });
  vi.stubGlobal('fetch', fetchMock);
});
afterEach(() => vi.unstubAllGlobals());

describe('безопасный повтор действий', () => {
  it('повтор после неопределённого сетевого результата использует прежний ключ, но не отправляется автоматически', async () => {
    const keys = new MutationKeys(() => 'stable-operation-id');
    const body = { title: 'Вопрос' };
    fetchMock.mockRejectedValueOnce(new TypeError('offline')).mockResolvedValueOnce(response(201, { id: 'saved-request' }));
    await expect(workflowWrite('/api/requests', 'session', body, keys.key('create', body))).rejects.toThrow('offline');
    expect(fetchMock).toHaveBeenCalledTimes(1);
    await expect(workflowWrite('/api/requests', 'session', body, keys.key('create', body))).resolves.toEqual({ id: 'saved-request' });
    expect(fetchMock.mock.calls.map(([, options]) => (options?.headers as Record<string, string>)['Idempotency-Key'])).toEqual(['stable-operation-id', 'stable-operation-id']);
  });
  it('изменённое содержимое, другое действие и завершённая операция получают отдельные ключи', () => {
    let serial = 0;
    const keys = new MutationKeys(() => `operation-${++serial}`);
    const first = keys.key('offer', { message: 'Могу объяснить' });
    expect(keys.key('offer', { message: 'Могу объяснить' })).toBe(first);
    expect(keys.key('offer', { message: 'Могу показать пример' })).not.toBe(first);
    const other = keys.key('report', { message: 'Могу объяснить' });
    expect(other).not.toBe(first);
    keys.clear('report');
    expect(keys.key('report', { message: 'Могу объяснить' })).not.toBe(other);
  });
  it('отправляет clientMessageId в теле сообщения без подмены ключом общего действия', async () => {
    const body = { clientMessageId: 'message-id', text: 'Повторяемое сообщение' };
    fetchMock.mockResolvedValue(response(201, { id: 'server-message' }));
    await workflowWrite('/api/conversations/chat/messages', 'session', body);
    const options = fetchMock.mock.calls[0][1];
    expect(options?.body).toBe(JSON.stringify(body));
    expect(options?.headers).not.toHaveProperty('Idempotency-Key');
  });
  it('передаёт ревизию 0 как условие редактирования, а не создаёт новую запись', async () => {
    fetchMock.mockResolvedValue(response(200, { revision: 1 }));
    await workflowWrite('/api/requests/draft', 'session', { title: 'Исправлено' }, undefined, 0);
    expect(fetchMock).toHaveBeenCalledWith('/api/requests/draft', expect.objectContaining({
      method: 'PUT', headers: expect.objectContaining({ 'If-Match': '"0"', Authorization: 'Bearer session' }),
    }));
  });
  it('после истечения входа сохраняет исходную ошибку и не повторяет запись сам', async () => {
    fetchMock.mockResolvedValue(response(401, { error: { code: 'UNAUTHORIZED' } }));
    await expect(workflowWrite('/api/requests', 'expired', { title: 'Черновик' }, 'key')).rejects.toMatchObject({ status: 401 });
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });
  it('чтение переписки не содержит тело или ключ записи и не использует куки', async () => {
    fetchMock.mockResolvedValue(response(200, { items: [], nextAfter: 0 }));
    await workflowRead('/api/conversations/chat/messages?after=0', 'session');
    const options = fetchMock.mock.calls[0][1];
    expect(options).toMatchObject({ method: 'GET', credentials: 'omit', cache: 'no-store' });
    expect(options?.body).toBeUndefined();
    expect(options?.headers).not.toHaveProperty('Idempotency-Key');
  });
});

describe('понятные ошибки сценария', () => {
  it.each([400, 401, 403, 404, 409, 428, 500])('не раскрывает текст серверной ошибки (%i)', status => {
    expect(workflowError(new ApiError(status, 'private-server-information'))).not.toContain('private-server-information');
  });
  it('при конфликте предлагает сначала проверить данные и сообщает о сохранённом тексте', () => {
    const text = workflowError(new ApiError(409, 'CONFLICT'));
    expect(text).toContain('Обновите данные');
    expect(text).toContain('текст сохранён');
  });
});
