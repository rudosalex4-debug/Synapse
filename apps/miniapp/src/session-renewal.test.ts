import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { needsSignIn, saveProfile } from './api';
import { profileInput } from './profile-model';
import { renewalErrorMessage, renewProfileSession, SessionRenewalError } from './session-renewal';
import type { Profile, Session } from './types';

const previous: Session = { token: 'expired-token', expiresAt: '2026-09-18T10:00:00Z', user: { id: 'anna-id', displayName: 'Анна', provenance: 'demo' } };
const renewed: Session = { ...previous, token: 'renewed-token', expiresAt: '2026-09-18T11:00:00Z' };
const baseline: Profile = { id: 'anna-id', revision: 7, displayName: 'Анна', bio: '', availableToHelp: true, maxActiveConversations: 2, competencies: [], provenance: 'demo' };
const fetchMock = vi.fn<typeof fetch>();
const response = (status: number, data: unknown) => ({ status, ok: status >= 200 && status < 300, json: async () => data }) as Response;

beforeEach(() => {
  fetchMock.mockReset();
  vi.stubGlobal('window', { setTimeout, clearTimeout });
  vi.stubGlobal('fetch', fetchMock);
});
afterEach(() => vi.unstubAllGlobals());

describe('повторный вход с несохранённым профилем', () => {
  it('повторяет вход за исходного учебного участника без чтения или записи профиля', async () => {
    const boris = { ...previous, user: { ...previous.user, id: 'boris-id' } };
    fetchMock.mockResolvedValueOnce(response(200, { ...boris, token: 'boris-new' }));
    const session = await renewProfileSession(boris, { kind: 'demo', persona: 'boris' });
    expect(session.token).toBe('boris-new');
    expect(fetchMock).toHaveBeenCalledExactlyOnceWith('/api/auth/demo', expect.objectContaining({ method: 'POST', body: JSON.stringify({ persona: 'boris' }) }));
  });

  it('после 401 сохраняет черновик и прежнюю revision; повторная запись требует отдельного действия', async () => {
    const draft = { ...profileInput(baseline), bio: 'Мой несохранённый текст' };
    fetchMock.mockResolvedValueOnce(response(401, { error: { code: 'UNAUTHORIZED' } }))
      .mockResolvedValueOnce(response(200, renewed))
      .mockResolvedValueOnce(response(409, { error: { code: 'PROFILE_CONFLICT' } }));
    const cause = await saveProfile(previous.token, draft, baseline.revision).catch((error: unknown) => error);
    expect(needsSignIn(cause)).toBe(true);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const session = await renewProfileSession(previous, { kind: 'demo', persona: 'anna' });
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(fetchMock.mock.calls.map(([path]) => path)).toEqual(['/api/profile', '/api/auth/demo']);
    expect(draft.bio).toBe('Мой несохранённый текст');
    expect(baseline.revision).toBe(7);

    // This explicit action can conflict with another device, even after renewal.
    await expect(saveProfile(session.token, draft, baseline.revision)).rejects.toMatchObject({ status: 409 });
    expect(fetchMock).toHaveBeenCalledTimes(3);
    expect(fetchMock).toHaveBeenNthCalledWith(3, '/api/profile', expect.objectContaining({
      method: 'PUT', headers: expect.objectContaining({ Authorization: 'Bearer renewed-token', 'If-Match': '"7"' }), body: JSON.stringify(draft),
    }));
  });

  it.each([
    { id: 'other-user', provenance: 'demo' },
    { id: 'anna-id', provenance: 'self_declared' },
  ])('не принимает аккаунт или источник, отличающийся от исходного: %j', async (user) => {
    fetchMock.mockResolvedValueOnce(response(200, { ...renewed, user: { ...previous.user, ...user } }))
      .mockResolvedValueOnce(response(200, { ok: true }));
    await expect(renewProfileSession(previous, { kind: 'demo', persona: 'anna' })).rejects.toMatchObject({ reason: 'different_user' });
    expect(fetchMock.mock.calls.map(([path]) => path)).toEqual(['/api/auth/demo', '/api/auth/logout']);
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/auth/logout', expect.objectContaining({ headers: expect.objectContaining({ Authorization: 'Bearer renewed-token' }) }));
  });

  it('даже при сбое отзыва чужого токена не принимает другой аккаунт', async () => {
    fetchMock.mockResolvedValueOnce(response(200, { ...renewed, user: { ...previous.user, id: 'other-user' } }))
      .mockRejectedValueOnce(new Error('offline'));
    await expect(renewProfileSession(previous, { kind: 'demo', persona: 'anna' })).rejects.toMatchObject({ reason: 'different_user' });
  });

  it('сбой сети не превращает в истечение сессии и не повторяет запрос', async () => {
    fetchMock.mockRejectedValueOnce(new Error('offline'));
    const cause = await renewProfileSession(previous, { kind: 'demo', persona: 'anna' }).catch((error: unknown) => error);
    expect(needsSignIn(cause)).toBe(false);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });
});

describe('повторный вход MAX', () => {
  const maxPrevious: Session = { ...previous, user: { ...previous.user, provenance: 'self_declared' } };
  it('при каждом действии читает текущую raw-строку bridge и сверяет серверный id', async () => {
    window.WebApp = { initData: 'old-signed-launch' };
    window.WebApp.initData = 'current-signed-launch';
    fetchMock.mockResolvedValueOnce(response(200, { ...maxPrevious, token: 'max-renewed' }));
    await expect(renewProfileSession(maxPrevious, { kind: 'max' })).resolves.toMatchObject({ token: 'max-renewed' });
    expect(fetchMock).toHaveBeenCalledExactlyOnceWith('/api/auth/max', expect.objectContaining({ body: JSON.stringify({ initData: 'current-signed-launch' }) }));
  });

  it('не использует неподписанный user или demo-вход, если launch-строка отсутствует', async () => {
    window.WebApp = Object.assign({ initData: '' }, { initDataUnsafe: { user: { id: 'anna-id' } } });
    await expect(renewProfileSession(maxPrevious, { kind: 'max' })).rejects.toMatchObject({ reason: 'max_reopen_required' });
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('при отклонении launch-строки объясняет повторное открытие и потерю черновика', async () => {
    window.WebApp = { initData: 'expired-signed-launch' };
    fetchMock.mockResolvedValueOnce(response(401, { error: { code: 'INIT_DATA_EXPIRED' } }));
    const cause = await renewProfileSession(maxPrevious, { kind: 'max' }).catch((error: unknown) => error);
    expect(cause).toBeInstanceOf(SessionRenewalError);
    expect(renewalErrorMessage(cause)).toContain('в том же аккаунте MAX');
    expect(renewalErrorMessage(cause)).toContain('При закрытии черновик будет потерян');
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it('при недоступности сервера оставляет возможность повторить вход в открытом приложении', async () => {
    window.WebApp = { initData: 'current-signed-launch' };
    fetchMock.mockResolvedValueOnce(response(503, { error: { code: 'NOT_READY' } }));
    const cause = await renewProfileSession(maxPrevious, { kind: 'max' }).catch((error: unknown) => error);
    expect(renewalErrorMessage(cause)).toContain('Повторите попытку');
    expect(renewalErrorMessage(cause)).not.toContain('закройте');
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });
});
