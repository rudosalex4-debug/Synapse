import { useCallback, useEffect, useRef, useState, type FormEvent } from 'react';
import { api, ApiError, errorMessage, needsSignIn } from './api';
import { MutationKeys } from './workflow-api';
import type { Session } from './types';

type Decision = 'dismiss' | 'close_conversation' | 'block_pair';
type Filter = 'new' | 'resolved' | 'all';
interface ReportSummary { id: string; status: 'new' | 'resolved'; revision: number; category: 'abuse' | 'spam' | 'other'; createdAt: string; reporterName: string; targetName: string; requestTitle: string; resolution: Decision | null; resolvedAt: string | null }
interface ReportDetail extends ReportSummary { text: string; conversationId: string; conversationStatus: 'active' | 'closed'; note: string | null }
interface ReportPage { items: ReportSummary[]; nextCursor: string | null }
interface EvidenceMessage { sequence: number; senderName: string; text: string; createdAt: string }
interface EvidencePage { items: EvidenceMessage[]; nextBeforeSequence: number | null }
const decisions: Record<Decision, string> = { dismiss: 'Закрыть жалобу без ограничений', close_conversation: 'Завершить указанный диалог', block_pair: 'Прекратить общение между этими участниками' };
const categories = { abuse: 'Оскорбления или давление', spam: 'Спам', other: 'Другое' };
const time = (value: string) => new Date(value).toLocaleString('ru-RU', { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });

/** Report text and evidence stay in component memory and are cleared when access or selection changes. */
export function ModerationWorkspace({ session, onSessionExpired, onRenewSession, onDirtyChange }: { session: Session; onSessionExpired: () => void; onRenewSession: () => Promise<void>; onDirtyChange: (dirty: boolean) => void }) {
  const [allowedToken, setAllowedToken] = useState<string | null>(null);
  const [open, setOpen] = useState(false);
  const [filter, setFilter] = useState<Filter>('new');
  const [page, setPage] = useState<ReportPage>({ items: [], nextCursor: null });
  const [detail, setDetail] = useState<ReportDetail | null>(null);
  const [evidence, setEvidence] = useState<EvidencePage | null>(null);
  const [decision, setDecision] = useState<Decision | ''>('');
  const [note, setNote] = useState('');
  const [loading, setLoading] = useState(false);
  const [loaded, setLoaded] = useState(false);
  const [pending, setPending] = useState('');
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [conflict, setConflict] = useState(false);
  const [accessLost, setAccessLost] = useState(false);
  const [expired, setExpired] = useState(false);
  const [accessAttempt, setAccessAttempt] = useState(0);
  const epoch = useRef(0);
  const queueGeneration = useRef(0);
  const busy = useRef(false);
  const everAllowed = useRef(false);
  const keys = useRef(new MutationKeys());
  const heading = useRef<HTMLHeadingElement>(null);
  const callbacks = useRef({ onSessionExpired }); callbacks.current = { onSessionExpired };
  const allowed = allowedToken === session.token;

  const clearPrivateData = useCallback(() => {
    setPage({ items: [], nextCursor: null }); setDetail(null); setEvidence(null);
    setNote(''); setDecision(''); setConflict(false); setNotice(''); setLoaded(false);
    keys.current = new MutationKeys();
  }, []);

  const failed = useCallback((cause: unknown) => {
    if (cause instanceof ApiError && cause.code === 'MODERATION_CONFLICT') {
      setError('Решение по своему диалогу должен принять другой ответственный.');
      return;
    }
    if (needsSignIn(cause) || (cause instanceof ApiError && cause.status === 403)) {
      ++epoch.current; ++queueGeneration.current;
      clearPrivateData(); setAllowedToken(null); setOpen(false); setAccessLost(true);
      setExpired(needsSignIn(cause)); setError('');
      if (needsSignIn(cause)) callbacks.current.onSessionExpired();
      return;
    }
    setError(cause instanceof ApiError && cause.status === 404 ? 'Жалоба больше недоступна. Обновите очередь.' : errorMessage(cause));
  }, [clearPrivateData]);

  useEffect(() => {
    const current = ++epoch.current;
    ++queueGeneration.current; clearPrivateData(); setAllowedToken(null); setLoading(false); setError(''); setAccessLost(false);
    void api<{ canModerate: boolean }>('/api/moderation/access', { token: session.token }).then(result => {
      if (current !== epoch.current) return;
      if (result.canModerate) { everAllowed.current = true; setAllowedToken(session.token); setAccessLost(false); setExpired(false); }
      else { setOpen(false); setAccessLost(everAllowed.current); }
    }).catch(cause => {
      if (current !== epoch.current) return;
      failed(cause); setAccessLost(true);
    });
    return () => { ++epoch.current; ++queueGeneration.current; };
  }, [session.token, accessAttempt, clearPrivateData, failed]);

  useEffect(() => { onDirtyChange(!!note.trim()); return () => onDirtyChange(false); }, [note, onDirtyChange]);

  useEffect(() => {
    const beforeUnload = (event: BeforeUnloadEvent) => { if (note.trim()) event.preventDefault(); };
    window.addEventListener('beforeunload', beforeUnload);
    return () => window.removeEventListener('beforeunload', beforeUnload);
  }, [note]);

  const loadQueue = useCallback(async (status: Filter, cursor?: string) => {
    if (allowedToken !== session.token) return;
    const current = epoch.current;
    const generation = ++queueGeneration.current;
    setLoading(true); setError('');
    try {
      const result = await api<ReportPage>(`/api/moderation/reports?status=${status}${cursor ? `&cursor=${encodeURIComponent(cursor)}` : ''}`, { token: session.token });
      if (current !== epoch.current || generation !== queueGeneration.current) return;
      setPage(previous => ({ ...result, items: cursor ? [...new Map([...previous.items, ...result.items].map(item => [item.id, item])).values()] : result.items })); setLoaded(true);
    } catch (cause) { if (current === epoch.current && generation === queueGeneration.current) failed(cause); }
    finally { if (current === epoch.current && generation === queueGeneration.current) setLoading(false); }
  }, [session.token, allowedToken, failed]);

  useEffect(() => { if (open && allowed) void loadQueue(filter); }, [open, allowed, filter, loadQueue]);
  useEffect(() => { if (detail) heading.current?.focus(); }, [detail?.id]);

  async function openReport(id: string, reload = false) {
    if (busy.current || !allowed) return;
    if (!reload && note.trim() && !window.confirm('Открыть другую жалобу? Несохранённое обоснование решения будет потеряно.')) return;
    const current = epoch.current;
    busy.current = true; setPending('detail'); setError(''); setEvidence(null);
    if (!reload) { setDetail(null); setNote(''); setDecision(''); setConflict(false); setNotice(''); }
    try {
      const result = await api<ReportDetail>(`/api/moderation/reports/${encodeURIComponent(id)}`, { token: session.token });
      if (current !== epoch.current) return;
      setDetail(result); setConflict(false); setDecision('');
      if (reload) setNotice(result.status === 'resolved' ? 'Жалобу уже рассмотрели. Ваше несохранённое обоснование осталось ниже.' : 'Актуальная версия загружена. Проверьте её и заново выберите решение.');
    } catch (cause) { if (current === epoch.current) failed(cause); }
    finally { busy.current = false; setPending(''); }
  }

  async function loadEvidence(before?: number) {
    if (!detail || busy.current || !allowed) return;
    const current = epoch.current;
    busy.current = true; setPending('evidence'); setError('');
    try {
      const result = await api<EvidencePage>(`/api/moderation/reports/${encodeURIComponent(detail.id)}/messages${before ? `?beforeSequence=${before}` : ''}`, { token: session.token });
      if (current !== epoch.current) return;
      setEvidence(previous => ({ ...result, items: before && previous ? [...new Map([...result.items, ...previous.items].map(item => [item.sequence, item])).values()].sort((a, b) => a.sequence - b.sequence) : result.items }));
    } catch (cause) { if (current === epoch.current) failed(cause); }
    finally { busy.current = false; setPending(''); }
  }

  async function submit(event: FormEvent) {
    event.preventDefault();
    if (!detail || detail.status !== 'new' || !decision || !note.trim() || conflict || busy.current || !allowed) return;
    if (decision !== 'dismiss' && !window.confirm(decision === 'close_conversation' ? 'Завершить этот диалог? Участники больше не смогут писать в нём.' : 'Прекратить общение этих двух участников? Их активные диалоги завершатся, новые сообщения и отклики между ними станут недоступны.')) return;
    const current = epoch.current;
    const body = { expectedRevision: detail.revision, decision, note: note.trim() };
    const action = `review-${detail.id}`;
    busy.current = true; setPending('decision'); setError(''); setNotice('');
    try {
      const result = await api<ReportDetail>(`/api/moderation/reports/${encodeURIComponent(detail.id)}/decision`, { token: session.token, body, idempotencyKey: keys.current.key(action, body) });
      if (current !== epoch.current) return;
      keys.current.clear(action); setDetail(result); setEvidence(null); setNote(''); setDecision(''); setNotice('Решение сохранено.');
      await loadQueue(filter);
    } catch (cause) {
      if (current !== epoch.current) return;
      if (cause instanceof ApiError && cause.status === 409) { setConflict(true); setEvidence(null); setError('Жалоба изменилась. Обоснование сохранено на экране. Загрузите актуальное решение перед дальнейшими действиями.'); }
      else failed(cause);
    } finally { busy.current = false; setPending(''); }
  }

  function close() {
    if (busy.current || (note.trim() && !window.confirm('Закрыть модерацию? Несохранённое обоснование решения будет потеряно.'))) return;
    ++epoch.current; ++queueGeneration.current; setOpen(false); setLoading(false); setError(''); clearPrivateData();
  }

  if (!allowed) return accessLost ? <div className="notice notice-error" role="alert"><span>{expired ? 'Для модерации нужно повторить вход. Содержимое жалоб скрыто.' : 'Доступ к модерации не подтверждён. Содержимое жалоб скрыто.'}</span><button className="text-button" type="button" disabled={!!pending} onClick={() => { if (expired) { setPending('renew'); void onRenewSession().catch(cause => setError(errorMessage(cause))).finally(() => setPending('')); } else setAccessAttempt(value => value + 1); }}>{expired ? 'Войти снова' : 'Проверить доступ'}</button>{error && <span>{error}</span>}</div> : null;

  return <section id="moderation" className="moderation-workspace" aria-label="Модерация сообщества">
    <div className="moderation-entry"><div><span className="eyebrow">ДЛЯ МОДЕРАТОРА</span><h3>Жалобы участников</h3></div><button type="button" className="button secondary small" aria-expanded={open} aria-controls="moderation-content" disabled={!!pending} onClick={() => open ? close() : setOpen(true)}>{open ? 'Закрыть модерацию' : 'Открыть модерацию'}</button></div>
    {open && <div id="moderation-content">
      <p className="workflow-hint">Открывайте переписку только для разбора жалобы. Просмотр сообщений фиксируется. Решения не блокируют аккаунт участника во всём сообществе.</p>
      <div className="moderation-toolbar"><label className="field">Показать<select value={filter} disabled={!!pending} onChange={event => { if (note.trim() && !window.confirm('Сменить список? Несохранённое обоснование будет потеряно.')) return; ++queueGeneration.current; clearPrivateData(); setFilter(event.target.value as Filter); }}><option value="new">Ожидают решения</option><option value="resolved">Рассмотренные</option><option value="all">Все жалобы</option></select></label><button type="button" className="text-button" disabled={loading || !!pending} onClick={() => void loadQueue(filter)}>Обновить очередь</button></div>
      {error && <div className="notice notice-error" role="alert"><span>{error}</span>{conflict && detail && <button type="button" className="text-button" disabled={!!pending} onClick={() => void openReport(detail.id, true)}>Загрузить актуальную версию</button>}</div>}
      {notice && <p className="notice" role="status">{notice}</p>}
      {loading && <p role="status">Загружаем очередь…</p>}
      {loaded && !loading && !error && !page.items.length && <p className="empty-state">{filter === 'new' ? 'Жалоб, ожидающих решения, нет.' : 'В этом списке пока нет жалоб.'}</p>}
      <div className="moderation-queue">{page.items.map(item => <button key={item.id} type="button" className={`moderation-summary ${detail?.id === item.id ? 'selected' : ''}`} disabled={!!pending} onClick={() => void openReport(item.id)}><span className="request-card-top"><strong>{categories[item.category]}</strong><time dateTime={item.createdAt}>{time(item.createdAt)}</time></span><span>{item.requestTitle}</span><small>{item.reporterName} → {item.targetName} · {item.status === 'new' ? 'Ожидает решения' : 'Рассмотрена'}</small></button>)}</div>
      {page.nextCursor && <button type="button" className="button secondary small" disabled={loading || !!pending} onClick={() => void loadQueue(filter, page.nextCursor!)}>Ещё жалобы</button>}
      {pending === 'detail' && <p role="status">Загружаем жалобу…</p>}
      {detail && <article className="moderation-detail" aria-label="Разбор жалобы"><h4 ref={heading} tabIndex={-1}>{detail.requestTitle}</h4><p className="workflow-byline">{detail.reporterName} пожаловался на {detail.targetName} · {categories[detail.category]}</p><p className="plain-text">{detail.text}</p><p className="workflow-hint">Диалог {detail.conversationStatus === 'active' ? 'открыт' : 'завершён'} · версия жалобы {detail.revision}</p>
        <div className="moderation-evidence"><div className="workflow-actions"><button type="button" className="button secondary small" disabled={!!pending} onClick={() => evidence ? setEvidence(null) : void loadEvidence()}>{pending === 'evidence' ? 'Загружаем переписку…' : evidence ? 'Скрыть переписку' : 'Открыть переписку для проверки'}</button>{evidence?.nextBeforeSequence && <button type="button" className="text-button" disabled={!!pending} onClick={() => void loadEvidence(evidence.nextBeforeSequence!)}>Более ранние сообщения</button>}</div>{evidence && <><p className="workflow-hint">Показаны {evidence.items.length} сообщений{evidence.nextBeforeSequence ? '; более ранние можно загрузить отдельно' : ''}.</p><ol className="evidence-list">{evidence.items.map(message => <li key={message.sequence}><div><strong>{message.senderName}</strong><time dateTime={message.createdAt}>{time(message.createdAt)}</time></div><p className="plain-text">{message.text}</p></li>)}</ol>{!evidence.items.length && <p>В диалоге нет сообщений.</p>}</>}</div>
        {detail.status === 'resolved' ? <div className="conversation-result"><h4>{detail.resolution ? decisions[detail.resolution] : 'Жалоба рассмотрена'}</h4>{detail.resolvedAt && <p>{time(detail.resolvedAt)}</p>}{detail.note && <p className="plain-text">{detail.note}</p>}{note && <details><summary>Ваше несохранённое обоснование</summary><p className="plain-text">{note}</p><button className="text-button" type="button" onClick={() => setNote('')}>Удалить черновик обоснования</button></details>}</div> : <form onSubmit={event => void submit(event)}><fieldset className="workflow-fields" disabled={!!pending || conflict}><label className="field">Решение<select required value={decision} onChange={event => setDecision(event.target.value as Decision | '')}><option value="">Выберите после рассмотрения</option>{Object.entries(decisions).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label><label className="field">Обоснование решения<textarea required minLength={1} maxLength={1000} rows={3} value={note} onChange={event => setNote(event.target.value)} /><small>Обязательно · {note.length} / 1000</small></label><button className="button primary" type="submit" disabled={!decision || !note.trim()}>{pending === 'decision' ? 'Сохраняем решение…' : 'Применить решение'}</button></fieldset></form>}
      </article>}
    </div>}
  </section>;
}
