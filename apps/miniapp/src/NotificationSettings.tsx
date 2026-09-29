import { useEffect, useRef, useState } from 'react';
import { api, errorMessage, needsSignIn } from './api';
import type { Session } from './types';

interface Settings { enabled: boolean; botStarted: boolean; deliveryAvailable: boolean }
export function NotificationSettings({ session, onSessionExpired, onRenewSession }: {
  session: Session; onSessionExpired: () => void; onRenewSession: () => Promise<void>;
}) {
  const [settings, setSettings] = useState<Settings>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [expired, setExpired] = useState(false);
  const [refresh, setRefresh] = useState(0);
  const epoch = useRef(0);
  const pending = useRef(false);
  useEffect(() => {
    const generation = ++epoch.current;
    setSettings(undefined); setError(''); setExpired(false);
    void api<Settings>('/api/notification-settings', { token: session.token })
      .then(value => { if (generation === epoch.current) setSettings(value); })
      .catch(cause => { if (generation === epoch.current) { setError(errorMessage(cause)); if (needsSignIn(cause)) setExpired(true); } });
    return () => { ++epoch.current; };
  }, [session.token, refresh]);
  async function toggle() {
    if (!settings || pending.current || expired) return;
    const generation = epoch.current;
    pending.current = true; setBusy(true); setError('');
    try {
      const value = await api<Settings>('/api/notification-settings', { token: session.token, method: 'PUT', body: { enabled: !settings.enabled } });
      if (generation === epoch.current) setSettings(value);
    } catch (cause) {
      if (generation === epoch.current) { setError(errorMessage(cause)); if (needsSignIn(cause)) { setExpired(true); onSessionExpired(); } }
    } finally { pending.current = false; setBusy(false); }
  }
  async function renew() {
    if (pending.current) return;
    pending.current = true; setBusy(true); setError('');
    try { await onRenewSession(); } catch (cause) { setError(errorMessage(cause)); }
    finally { pending.current = false; setBusy(false); }
  }
  return <section className="notification-settings" aria-labelledby="notification-title">
    <h3 id="notification-title">Не пропустить ответ</h3>
    <p>Бот может сообщать о новых откликах, выборе вас помощником и сообщениях в диалоге. Текст переписки остаётся внутри приложения.</p>
    {settings && <>
      <p>{settings.enabled ? 'Вы разрешили уведомления в MAX.' : 'Уведомления выключены. Их можно включить по желанию.'}</p>
      {!settings.deliveryAvailable && <p className="workflow-hint">Отправка ещё не подключена для этого запуска. Ваш выбор сохранится; вопросы и диалоги доступны в приложении.</p>}
      {!settings.botStarted && <p className="workflow-hint">Для получения уведомлений откройте чат с ботом, нажмите «Старт», затем обновите статус здесь.</p>}
      <div className="workflow-actions"><button type="button" className="button secondary small" disabled={busy || expired} onClick={() => void toggle()}>{busy ? 'Сохраняем…' : settings.enabled ? 'Выключить уведомления' : 'Включить уведомления'}</button><button type="button" className="text-button" disabled={busy} onClick={() => setRefresh(value => value + 1)}>Обновить статус</button></div>
    </>}
    {!settings && !error && <p role="status">Загружаем настройки…</p>}
    {error && <div className="notice notice-error" role="alert"><span>{error}</span>{expired ? <button type="button" className="text-button" disabled={busy} onClick={() => void renew()}>Войти снова</button> : <button type="button" className="text-button" disabled={busy} onClick={() => setRefresh(value => value + 1)}>Повторить</button>}</div>}
  </section>;
}
