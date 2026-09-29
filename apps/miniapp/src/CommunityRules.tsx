import { useEffect, useRef, useState } from 'react';
import { api, ApiError, errorMessage, needsSignIn } from './api';
import type { Session } from './types';

interface Rules { version: string; items: { id: string; text: string }[] }
interface Acceptance { version: string; accepted: boolean; acceptedAt: string | null }

export function CommunityRules({ session, refreshKey, onStatusChange, onSessionExpired, onRenewSession }: { session: Session; refreshKey: number; onStatusChange: (token: string, accepted: boolean) => void; onSessionExpired: () => void; onRenewSession: () => Promise<void> }) {
  const [rules, setRules] = useState<Rules | null>(null);
  const [accepted, setAccepted] = useState(false);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [expired, setExpired] = useState(false);
  const [error, setError] = useState('');
  const [attempt, setAttempt] = useState(0);
  const generation = useRef(0);
  const pending = useRef(false);
  const callbacks = useRef({ onStatusChange, onSessionExpired });
  callbacks.current = { onStatusChange, onSessionExpired };

  useEffect(() => {
    const current = ++generation.current;
    setLoading(true); setSaving(false); setRules(null); setAccepted(false); setError(''); setExpired(false);
    callbacks.current.onStatusChange(session.token, false);
    void Promise.all([
      api<Rules>('/api/community-rules'),
      api<Acceptance>('/api/community-rules/status', { token: session.token }),
    ]).then(([copy, status]) => {
      if (current !== generation.current) return;
      if (!copy.items.length || status.version !== copy.version) throw new Error('Rules version changed');
      setRules(copy); setAccepted(status.accepted);
      callbacks.current.onStatusChange(session.token, status.accepted);
    }).catch(cause => {
      if (current !== generation.current) return;
      setError(errorMessage(cause));
      if (needsSignIn(cause)) { setExpired(true); callbacks.current.onSessionExpired(); }
    }).finally(() => { if (current === generation.current) setLoading(false); });
    return () => { ++generation.current; };
  }, [session.token, attempt, refreshKey]);

  async function accept() {
    if (!rules || pending.current || loading || expired || accepted) return;
    const current = generation.current;
    pending.current = true; setSaving(true); setError('');
    try {
      const status = await api<Acceptance>('/api/community-rules/accept', { token: session.token, body: { version: rules.version } });
      if (current !== generation.current) return;
      if (!status.accepted || status.version !== rules.version) { setAttempt(value => value + 1); return; }
      setAccepted(true); callbacks.current.onStatusChange(session.token, true);
    } catch (cause) {
      if (current !== generation.current) return;
      if (cause instanceof ApiError && cause.code === 'RULES_VERSION_CONFLICT') { setRules(null); setError('Правила обновились. Загрузите актуальную версию, прочитайте её и подтвердите согласие.'); }
      else setError(errorMessage(cause));
      if (needsSignIn(cause)) { setExpired(true); callbacks.current.onSessionExpired(); }
    } finally { pending.current = false; if (current === generation.current) setSaving(false); }
  }

  async function renew() {
    if (pending.current) return;
    pending.current = true; setSaving(true); setError('');
    try { await onRenewSession(); }
    catch (cause) { setError(errorMessage(cause)); }
    finally { pending.current = false; setSaving(false); }
  }

  return <section id="community-rules" className={`community-rules ${accepted ? 'rules-accepted' : ''}`} aria-labelledby="community-rules-title" aria-busy={loading || saving}>
    <div className="section-heading compact"><div><span className="eyebrow">ДОГОВОРЁННОСТЬ УЧАСТНИКОВ</span><h3 id="community-rules-title">{accepted ? 'Правила сообщества приняты' : 'Прежде чем начать общение'}</h3></div>{accepted && <span aria-hidden="true">✓</span>}</div>
    {loading && <p role="status">Загружаем правила и проверяем, приняли ли вы их…</p>}
    {!accepted && !loading && <p>Прочитайте правила и подтвердите согласие. После этого можно задавать вопросы, откликаться и писать собеседнику. История, жалобы и блокировка доступны уже сейчас.</p>}
    {rules && (accepted ? <details><summary>Посмотреть правила · версия {rules.version}</summary><ul>{rules.items.map(item => <li key={item.id}>{item.text}</li>)}</ul></details> : <ul>{rules.items.map(item => <li key={item.id}>{item.text}</li>)}</ul>)}
    {error && <div className="notice notice-error" role="alert"><span>{error}</span><button type="button" className="text-button" disabled={loading || saving} onClick={() => expired ? void renew() : setAttempt(value => value + 1)}>{expired ? 'Войти снова' : 'Повторить загрузку'}</button></div>}
    {!accepted && rules && !loading && <button type="button" className="button primary" disabled={saving || expired} onClick={() => void accept()}>{saving ? 'Сохраняем согласие…' : 'Принимаю правила сообщества'}</button>}
  </section>;
}
