import { ApiError, api, errorMessage, logoutSession } from './api';
import type { Session } from './types';

export type LoginSource = { kind: 'demo'; persona: 'anna' | 'boris' } | { kind: 'max' };

export class SessionRenewalError extends Error {
  constructor(public readonly reason: 'max_reopen_required' | 'different_user') { super(reason); }
}

// This operation only renews authentication. The caller retains the profile,
// its original revision and every unsaved edit until an explicit save or reload.
export async function renewProfileSession(previous: Session, source: LoginSource): Promise<Session> {
  let next: Session;
  if (source.kind === 'demo') {
    next = await api<Session>('/api/auth/demo', { body: { persona: source.persona } });
  } else {
    // Read the current signed launch string; never infer identity from bridge user data.
    const initData = window.WebApp?.initData;
    if (typeof initData !== 'string' || !initData.trim()) throw new SessionRenewalError('max_reopen_required');
    try {
      next = await api<Session>('/api/auth/max', { body: { initData } });
    } catch (cause) {
      if (cause instanceof ApiError && cause.status === 401) throw new SessionRenewalError('max_reopen_required');
      throw cause;
    }
  }
  if (!next.user?.id || next.user.id !== previous.user.id || next.user.provenance !== previous.user.provenance) {
    // A returned session for another account must never be applied to this draft.
    if (next.token) await logoutSession(next.token).catch(() => undefined);
    throw new SessionRenewalError('different_user');
  }
  return next;
}

export function renewalErrorMessage(error: unknown): string {
  if (error instanceof SessionRenewalError) {
    if (error.reason === 'different_user') return 'Вход выполнен в другой аккаунт. Черновик остался на экране и не отправлен. Вернитесь в исходный аккаунт и повторите вход; для смены участника сначала выйдите из профиля.';
    return 'Данные запуска MAX отсутствуют или устарели. Скопируйте несохранённые изменения, затем закройте мини-приложение и откройте его через бота в том же аккаунте MAX. При закрытии черновик будет потерян.';
  }
  return errorMessage(error);
}
