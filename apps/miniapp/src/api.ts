import type { Profile, ProfileInput } from './types';

export class ApiError extends Error {
  constructor(public readonly status: number, public readonly code: string, public readonly safeMessage?: string) { super(code); }
}
export async function api<T>(path: string, options: { token?: string; body?: unknown; method?: string; revision?: number; idempotencyKey?: string } = {}): Promise<T> {
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), 15_000);
  try {
    const response = await fetch(path, {
      method: options.method ?? (options.body === undefined ? 'GET' : 'POST'),
      headers: {
        Accept: 'application/json',
        ...(options.idempotencyKey ? { 'Idempotency-Key': options.idempotencyKey } : {}),
        ...(options.body === undefined ? {} : { 'Content-Type': 'application/json' }),
        ...(options.token ? { Authorization: `Bearer ${options.token}` } : {}),
        ...(options.revision === undefined ? {} : { 'If-Match': `"${options.revision}"` }),
      },
      body: options.body === undefined ? undefined : JSON.stringify(options.body),
      signal: controller.signal,
      credentials: 'omit',
      cache: 'no-store',
    });
    if (response.status === 204) return undefined as T;
    const data = await response.json().catch(() => null);
    if (!response.ok) {
      const code = typeof data?.error?.code === 'string' ? data.error.code : 'request_failed';
      // Only the pilot access response has a deliberately public, actionable message.
      const safeMessage = code === 'PILOT_ACCESS_DENIED' && typeof data?.error?.message === 'string'
        ? data.error.message.slice(0, 500) : undefined;
      throw new ApiError(response.status, code, safeMessage);
    }
    if (data === null) throw new ApiError(502, 'invalid_response');
    return data as T;
  } finally { window.clearTimeout(timeout); }
}
export function saveProfile(token: string, body: ProfileInput, revision: number): Promise<Profile> {
  return api<Profile>('/api/profile', { token, method: 'PUT', body, revision });
}
export async function logoutSession(token: string): Promise<void> {
  try { await api<void>('/api/auth/logout', { token, method: 'POST' }); }
  catch (cause) { if (!(cause instanceof ApiError && cause.status === 401)) throw cause; }
}
export function needsSignIn(error: unknown): boolean {
  return error instanceof ApiError && error.status === 401;
}
export function needsProfileReload(error: unknown): boolean {
  return error instanceof ApiError && (error.status === 409 || error.status === 428);
}
export function errorMessage(error: unknown): string {
  if (error instanceof ApiError) {
    if (error.code === 'PILOT_ACCESS_DENIED') return error.safeMessage || 'Пока доступ открыт только участникам закрытого пилота. Передайте организатору свой MAX ID для приглашения.';
    if (error.status === 401) return 'Не удалось подтвердить вход. Откройте приложение заново или повторите демонстрационный вход.';
    if (error.status === 403) return 'Этот способ входа недоступен. Откройте мини-приложение через бота в MAX.';
    if (error.status === 409) return 'Профиль изменён в другом окне или на другом устройстве. Ваши изменения остались на экране. Загрузите актуальный профиль, затем внесите их заново.';
    if (error.status === 428) return 'Нужно загрузить актуальный профиль перед сохранением. Ваши изменения остались на экране.';
    if (error.status === 400 || error.status === 422) return 'Не удалось сохранить данные. Проверьте заполненные поля и выбранные темы.';
    if (error.status === 429) return 'Слишком много запросов. Подождите немного и повторите попытку.';
    if (error.status === 503) return 'Сервис пока не готов. Повторите попытку немного позже.';
    return 'Сервер не смог выполнить запрос. Ваши изменения остались на экране — попробуйте ещё раз.';
  }
  if (error instanceof Error && error.name === 'AbortError') return 'Сервис долго не отвечает. Проверьте подключение и повторите попытку.';
  return 'Не удалось связаться с сервисом. Проверьте подключение и повторите попытку.';
}
