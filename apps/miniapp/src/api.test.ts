import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError, api, errorMessage, logoutSession, needsProfileReload, saveProfile } from './api';
import type { ProfileInput } from './types';

const draft: ProfileInput = { displayName: 'Анна', bio: '', availableToHelp: true, maxActiveConversations: 2, competencies: [] };
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

describe('сохранение с версией профиля', () => {
  it('передаёт исходную версию в If-Match и использует новую версию следующего сохранения', async () => {
    const next = { ...draft, id: 'anna', provenance: 'demo', revision: 8 };
    fetchMock.mockResolvedValueOnce(response(200, next)).mockResolvedValueOnce(response(200, { ...next, revision: 9 }));
    const saved = await saveProfile('session-token', draft, 7);
    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/profile', expect.objectContaining({
      method: 'PUT', headers: expect.objectContaining({ 'If-Match': '"7"', Authorization: 'Bearer session-token' }), body: JSON.stringify(draft),
    }));
    expect(saved.revision).toBe(8);
    await saveProfile('session-token', draft, saved.revision);
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/profile', expect.objectContaining({ headers: expect.objectContaining({ 'If-Match': '"8"' }) }));
  });
  it('не добавляет If-Match при чтении профиля', async () => {
    fetchMock.mockResolvedValue(response(200, { ...draft, revision: 7 }));
    await api('/api/profile', { token: 'session-token' });
    expect(fetchMock.mock.calls[0][1]?.headers).not.toHaveProperty('If-Match');
  });
  it.each([409, 428])('показывает необходимость загрузки без автоматической перезаписи при %i', async (status) => {
    fetchMock.mockResolvedValue(response(status, { error: { code: 'PROFILE_CONFLICT' } }));
    const error = await saveProfile('session-token', draft, 7).catch((cause: unknown) => cause);
    expect(needsProfileReload(error)).toBe(true);
    expect(errorMessage(error)).toContain('изменения остались на экране');
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });
  it('не принимает сетевую ошибку за конфликт версий', () => {
    expect(needsProfileReload(new Error('offline'))).toBe(false);
    expect(needsProfileReload(new ApiError(500, 'FAILED'))).toBe(false);
  });
});

describe('завершение сессии', () => {
  it('отзывает текущий bearer-токен на сервере', async () => {
    fetchMock.mockResolvedValue(response(200, { ok: true }));
    await logoutSession('current-session');
    expect(fetchMock).toHaveBeenCalledWith('/api/auth/logout', expect.objectContaining({
      method: 'POST', headers: expect.objectContaining({ Authorization: 'Bearer current-session' }), credentials: 'omit', cache: 'no-store',
    }));
  });
  it('позволяет завершить уже истёкшую сессию', async () => {
    fetchMock.mockResolvedValue(response(401, { error: { code: 'UNAUTHORIZED' } }));
    await expect(logoutSession('expired-session')).resolves.toBeUndefined();
  });
  it('сообщает о сбое отзыва, чтобы интерфейс не объявлял сессию завершённой', async () => {
    fetchMock.mockResolvedValue(response(503, { error: { code: 'NOT_READY' } }));
    await expect(logoutSession('current-session')).rejects.toMatchObject({ status: 503 });
  });
});
