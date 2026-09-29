import { useCallback, useEffect, useMemo, useRef, useState, type FormEvent } from 'react';
import { ApiError, errorMessage, needsSignIn } from './api';
import { buildCatalogIndex } from './catalog-index';
import { topicPath } from './profile-model';
import { TopicJourney } from './TopicJourney';
import { ContextFields } from './ContextFields';
import { renewalErrorMessage } from './session-renewal';
import type { Profile, Session, Taxonomy } from './types';
import { MutationKeys, workflowError, workflowRead, workflowWrite, type Conversation, type CreateRequest, type HelpRequest, type Message, type MessagePage, type Offer, type Outcome, type ReportCategory, type RequestPage, type RequestReach, type MatchExplanation } from './workflow-api';
import { changeRequestTopic, experienceLabels, goalLabels, mergeMessages, newRequest, outcomeLabels, requestInput, requestValidation, shouldPoll, statusLabels } from './workflow-model';

type Tab = 'mine' | 'feed' | 'conversations';
const tabs = [['mine', 'Мои вопросы'], ['feed', 'Могу помочь'], ['conversations', 'Диалоги']] as const;
type EditorState = { draft: CreateRequest; original?: HelpRequest };
const emptyPage: RequestPage = { items: [], nextCursor: null };
const offerLabels = { pending: 'Отклик отправлен', accepted: 'Помощник выбран', declined: 'Выбран другой помощник', withdrawn: 'Отклик отозван' } as const;
const time = (value: string) => new Date(value).toLocaleString('ru-RU', { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });

function MatchReasons({ explanation, taxonomy }: { explanation?: MatchExplanation | null; taxonomy: Taxonomy }) {
  if (!explanation) return null;
  const labels = (ids: string[]) => ids.map(id => taxonomy.facets.find(facet => facet.id === id)?.label ?? id).join(', ');
  return <div className="match-explanation">
    <p><strong>Почему подходит</strong> · {explanation.topicRelation === 'narrower' ? 'Опыт охватывает часть темы - уточните границы помощи.' : 'Тема совпадает с указанным знанием.'}</p>
    {!!explanation.matchedFacets.length && <p>Совпали уточнения: {labels(explanation.matchedFacets)}.</p>}
    {!!explanation.missingOptionalFacets.length && <p>Не совпали необязательные пожелания: {labels(explanation.missingOptionalFacets)}. Это можно обсудить.</p>}
    {explanation.experience !== 'not_requested' && <p>{explanation.experience === 'matched' ? 'Вид опыта совпадает с пожеланием автора.' : 'Указан другой вид опыта; он не был обязательным условием.'}</p>}
    <small>Сопоставление основано на знаниях, указанных участником. Это не подтверждение квалификации.</small>
  </div>;
}

function RequestReachPanel({ request, reach, hasUnsavedChanges, busy, checking, parentLabel, onCheck, onEdit }: { request: HelpRequest; reach?: RequestReach; hasUnsavedChanges: boolean; busy: boolean; checking: boolean; parentLabel: string; onCheck: () => void; onEdit: () => void }) {
  const stale = !!reach && (reach.requestRevision !== request.revision || hasUnsavedChanges);
  const status = reach?.status === 'available' ? 'Сейчас есть подходящие участники'
    : reach?.status === 'limited' ? 'В проверенной части сообщества совпадений пока нет'
      : 'Сейчас не нашли подходящих участников';
  return <section className="request-reach" aria-label="Предварительный подбор">
    <div className="request-reach-heading"><h4>Как вопрос находит помощь</h4><button type="button" className="button secondary small" disabled={busy || hasUnsavedChanges} onClick={onCheck}>{checking ? 'Проверяем…' : 'Проверить подбор'}</button></div>
    <p className="workflow-hint">Проверка учитывает сохранённую тему, обязательные условия и готовность участников помогать. Публикация доступна независимо от результата.</p>
    {hasUnsavedChanges && <p className="reach-note">Сначала сохраните изменения вопроса: проверка использует сохранённую версию.</p>}
    {reach && <div className={stale ? 'reach-result is-stale' : 'reach-result'} role="status">
      {stale ? <p><strong>Результат относится к предыдущей версии вопроса.</strong> После сохранения проверьте подбор ещё раз.</p> : <><p><strong>{status}</strong></p><p>{reach.status === 'available' ? 'Участники сами решают, откликаться ли на вопрос. Наличие совпадения не обещает ответ.' : reach.status === 'limited' ? 'Проверка охватила часть подходящих профилей. По этому результату нельзя судить обо всём сообществе.' : 'Состав сообщества и доступность людей меняются. Вопрос можно опубликовать и проверить подбор позже.'}</p>{reach.sampleLimited && reach.status === 'available' && <p>Проверена часть подходящих профилей, а не всё сообщество.</p>}</>}
      <small>Проверено {time(reach.checkedAt)} · сохранённая версия {reach.requestRevision}</small>
      {!stale && request.status === 'draft' && !!reach.suggestions.length && <div className="reach-suggestions"><p>Что можно уточнить перед публикацией:</p><ul>
        {reach.suggestions.includes('add_attempt') && <li>Расскажите, что уже пробовали и на каком шаге возникло затруднение.</li>}
        {reach.suggestions.includes('review_required_facets') && <li>Пересмотрите обязательные условия: оставьте только те, без которых помощь не подойдёт.</li>}
        {reach.suggestions.includes('choose_parent_topic') && reach.parentTopicId && <li>Если вопрос действительно шире, рассмотрите тему «{parentLabel}».</li>}
      </ul><button type="button" className="text-button" disabled={busy} onClick={onEdit}>Уточнить черновик</button></div>}
    </div>}
  </section>;
}

function RequestEditor({ editor, taxonomy, busy, writesAllowed, saveBlocked, onChange, onSave, onCancel }: { editor: EditorState; taxonomy: Taxonomy; busy: boolean; writesAllowed: boolean; saveBlocked: boolean; onChange: (draft: CreateRequest) => void; onSave: () => void; onCancel: () => void }) {
  const [validation, setValidation] = useState('');
  const catalog = useMemo(() => buildCatalogIndex(taxonomy), [taxonomy]);
  const draft = editor.draft;
  const path = catalog.path(draft.topicId);
  const skill = path.find(topic => topic.level === 2 && topic.active)?.id ?? '';
  function update(next: CreateRequest) { onChange(next); setValidation(''); }
  function changeTopic(id: string) { update(changeRequestTopic(draft, id, taxonomy)); }
  function submit(event: FormEvent) { event.preventDefault(); const error = requestValidation(draft, taxonomy); setValidation(error); if (!error && !saveBlocked) onSave(); }
  return <form className="question-editor" onSubmit={submit} aria-label="Редактор вопроса"><fieldset disabled={busy || !writesAllowed} className="workflow-fields">
    <div className="section-heading compact"><div><span className="eyebrow">НАЧНИТЕ С ВОПРОСА</span><h3>{editor.original ? 'Изменить черновик' : 'Новый вопрос'}</h3></div><button type="button" className="icon-button" aria-label="Закрыть редактор вопроса" onClick={onCancel}>×</button></div>
    <label className="field">Коротко о вопросе<input autoFocus value={draft.title} maxLength={120} required onChange={event => update({ ...draft, title: event.target.value })} placeholder="Например: как подготовиться к первой стажировке?" /></label>
    <label className="field">В чём нужна помощь<textarea rows={5} value={draft.body} minLength={30} maxLength={2000} required onChange={event => update({ ...draft, body: event.target.value })} placeholder="Опишите задачу и место, где возникло затруднение. Не добавляйте телефон, адрес или другие личные данные." /><small>{[...draft.body].length} / 2000 · Не менее 30 символов</small></label>
    <div className="field-grid"><label className="field">Чего хотите достичь<select value={draft.learningGoal} onChange={event => update({ ...draft, learningGoal: event.target.value as CreateRequest['learningGoal'] })}>{Object.entries(goalLabels).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label><label className="field">Какой опыт будет полезен<select value={draft.desiredExperience ?? ''} onChange={event => { const next = { ...draft }; if (event.target.value) next.desiredExperience = event.target.value as CreateRequest['desiredExperience']; else delete next.desiredExperience; update(next); }}><option value="">Любой подходящий</option>{Object.entries(experienceLabels).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label></div>
    <label className="field">Что уже пробовали <span className="optional">необязательно</span><textarea rows={3} value={draft.attempt ?? ''} maxLength={1000} onChange={event => update({ ...draft, attempt: event.target.value })} placeholder="Что получилось, а что осталось непонятным?" /></label>
    <TopicJourney taxonomy={taxonomy} value={draft.topicId} onChange={changeTopic} purpose="question" />
    {skill && <ContextFields taxonomy={taxonomy} topicId={draft.topicId} value={draft.facets} requiredFacets={draft.requiredFacets} onChange={(id, values) => { const facets = { ...draft.facets }; if (values.length) facets[id] = values; else delete facets[id]; update({ ...draft, facets, requiredFacets: values.length ? draft.requiredFacets : draft.requiredFacets.filter(facetId => facetId !== id) }); }} onRequiredChange={(id, required) => update({ ...draft, requiredFacets: required ? [...new Set([...draft.requiredFacets, id])] : draft.requiredFacets.filter(facetId => facetId !== id) })} />}
    <p className="workflow-hint">Уточнения помогут найти подходящий опыт. Обязательные условия сужают круг помощников. Черновик виден только вам; публикация будет отдельным действием.</p>
    {validation && <p className="notice notice-error" role="alert">{validation}</p>}
    <div className="workflow-actions"><button type="submit" className="button primary" disabled={saveBlocked || !skill}>{busy ? 'Сохраняем…' : 'Сохранить черновик'}</button><button type="button" className="button quiet" onClick={onCancel}>Отмена</button></div>
  </fieldset></form>;
}

export function Workspace({ session, taxonomy, profile, rulesAccepted, onRulesRequired, onRenewSession, onSessionExpired, onDirtyChange }: { session: Session; taxonomy: Taxonomy; profile: Profile; rulesAccepted: boolean; onRulesRequired: () => void; onRenewSession: () => Promise<void>; onSessionExpired: () => void; onDirtyChange: (dirty: boolean) => void }) {
  const [tab, setTab] = useState<Tab>('mine');
  const [pages, setPages] = useState<Record<'mine' | 'feed', RequestPage>>({ mine: emptyPage, feed: emptyPage });
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [selectedRequest, setSelectedRequest] = useState<HelpRequest | null>(null);
  const [offers, setOffers] = useState<Offer[]>([]);
  const [selectedConversationId, setSelectedConversationId] = useState<string | null>(null);
  const [messages, setMessages] = useState<Record<string, Message[]>>({});
  const [editor, setEditor] = useState<EditorState | null>(null);
  const [draftConflict, setDraftConflict] = useState<{ id: string; latest?: HelpRequest } | null>(null);
  const [offerDrafts, setOfferDrafts] = useState<Record<string, string>>({});
  const [chatDrafts, setChatDrafts] = useState<Record<string, string>>({});
  const [closeForm, setCloseForm] = useState<{ conversationId: string; outcome: Outcome; comment: string; nextStep: string } | null>(null);
  const [reachResults, setReachResults] = useState<Record<string, RequestReach>>({});
  const [reportForm, setReportForm] = useState<{ conversationId: string; category: ReportCategory; text: string } | null>(null);
  const [pending, setPending] = useState('');
  const pendingRef = useRef(false);
  const [loading, setLoading] = useState(false);
  const [listReady, setListReady] = useState<Record<Tab, boolean>>({ mine: false, feed: false, conversations: false });
  const [detailLoading, setDetailLoading] = useState('');
  const [messagesReady, setMessagesReady] = useState<Record<string, boolean>>({});
  const [messageLoading, setMessageLoading] = useState('');
  const [messageAttempt, setMessageAttempt] = useState(0);
  const [newMessagesBelow, setNewMessagesBelow] = useState(false);
  const messageList = useRef<HTMLOListElement>(null);
  const stickToBottom = useRef(true);
  const requestHeading = useRef<HTMLHeadingElement>(null);
  const launchHandled = useRef(false);
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [expired, setExpired] = useState(false);
  const [renewing, setRenewing] = useState(false);
  const [lastRefresh, setLastRefresh] = useState<Record<string, string>>({});
  const keys = useRef(new MutationKeys());
  const sessionRef = useRef(session);
  sessionRef.current = session;
  const rulesCallback = useRef(onRulesRequired); rulesCallback.current = onRulesRequired;
  const expiredCallback = useRef(onSessionExpired); expiredCallback.current = onSessionExpired;
  const cursor = useRef<Record<string, number>>({});
  const listGeneration = useRef(0);
  const detailGeneration = useRef(0);
  const mounted = useRef(true);
  const catalog = useMemo(() => buildCatalogIndex(taxonomy), [taxonomy]);
  const topicLabel = (id: string) => topicPath(id, taxonomy.topics).map(topic => topic.label).join(' → ') || id;
  const selectedConversation = conversations.find(conversation => conversation.id === selectedConversationId);
  const busy = !!pending || expired || renewing;
  const writeBlocked = busy || !rulesAccepted;
  const editorDirty = !!editor && JSON.stringify(editor.draft) !== JSON.stringify(editor.original ? requestInput(editor.original) : newRequest(taxonomy.version));
  const dirty = editorDirty || Object.values(offerDrafts).some(Boolean) || Object.values(chatDrafts).some(Boolean) || !!closeForm?.comment || !!closeForm?.nextStep || !!reportForm?.text;
  const dirtyRef = useRef(dirty); dirtyRef.current = dirty;

  useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);
  useEffect(() => { onDirtyChange(dirty); return () => onDirtyChange(false); }, [dirty, onDirtyChange]);
  useEffect(() => { const beforeUnload = (event: BeforeUnloadEvent) => { if (dirtyRef.current) event.preventDefault(); }; window.addEventListener('beforeunload', beforeUnload); return () => window.removeEventListener('beforeunload', beforeUnload); }, []);
  useEffect(() => { setExpired(false); }, [session.token]);

  const failed = useCallback((cause: unknown) => {
    if (!mounted.current) return;
    if (cause instanceof ApiError && cause.code === 'RULES_ACCEPTANCE_REQUIRED') rulesCallback.current();
    setError(cause instanceof ApiError && cause.code === 'RULES_ACCEPTANCE_REQUIRED' ? 'Чтобы продолжить, примите актуальные правила сообщества в карточке выше. Набранный текст остался на экране.' : cause instanceof ApiError && cause.code === 'PILOT_ACCESS_DENIED' ? errorMessage(cause) : workflowError(cause));
    if (needsSignIn(cause)) { setExpired(true); expiredCallback.current(); }
  }, []);

  const loadList = useCallback(async (scope: Tab, after?: string) => {
    const generation = ++listGeneration.current;
    setLoading(true);
    try {
      if (scope === 'conversations') {
        const data = await workflowRead<{ items: Conversation[] }>('/api/conversations', sessionRef.current.token);
        if (mounted.current && generation === listGeneration.current) setConversations(data.items);
      } else {
        const data = await workflowRead<RequestPage>(`/api/requests?scope=${scope}&limit=20${after ? `&after=${encodeURIComponent(after)}` : ''}`, sessionRef.current.token);
        if (mounted.current && generation === listGeneration.current) setPages(previous => ({ ...previous, [scope]: { ...data, items: after ? [...new Map([...previous[scope].items, ...data.items].map(item => [item.id, item])).values()] : data.items } }));
      }
      if (mounted.current && generation === listGeneration.current) setListReady(previous => ({ ...previous, [scope]: true }));
    } catch (cause) { if (generation === listGeneration.current) failed(cause); }
    finally { if (mounted.current && generation === listGeneration.current) setLoading(false); }
  }, [failed]);

  useEffect(() => { if (!expired) void loadList(tab); }, [tab, session.token, expired, loadList]);

  async function loadRequest(id: string, navigate = false) {
    const generation = ++detailGeneration.current;
    setDetailLoading(id);
    if (selectedRequest?.id !== id) { setError(''); setSelectedRequest(null); setOffers([]); }
    try {
      const request = await workflowRead<HelpRequest>(`/api/requests/${id}`, sessionRef.current.token);
      const nextOffers = await workflowRead<{ items: Offer[] }>(`/api/requests/${id}/offers`, sessionRef.current.token);
      if (mounted.current && generation === detailGeneration.current) { setSelectedRequest(request); setOffers(nextOffers.items); if (navigate) setTab(request.authorId === sessionRef.current.user.id ? 'mine' : 'feed'); }
    } catch (cause) { if (generation === detailGeneration.current) failed(cause); }
    finally { if (mounted.current && generation === detailGeneration.current) setDetailLoading(''); }
  }

  useEffect(() => { if (selectedRequest) requestHeading.current?.focus(); }, [selectedRequest?.id]);
  useEffect(() => {
    if (launchHandled.current) return;
    launchHandled.current = true;
    // The launch parameter is only a navigation hint. The authenticated GET decides access.
    const hint = new URLSearchParams(window.WebApp?.initData ?? '').get('start_param') ?? '';
    const match = /^request_([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$/i.exec(hint);
    if (match) { setTab('feed'); void loadRequest(match[1], true); }
  }, []);

  async function checkReach(request: HelpRequest) {
    if (pendingRef.current || expired || renewing || (editor?.original?.id === request.id && editorDirty)) return;
    pendingRef.current = true; setPending(`reach-${request.id}`); setError('');
    try {
      const result = await workflowRead<RequestReach>(`/api/requests/${request.id}/reach`, sessionRef.current.token);
      if (mounted.current) setReachResults(previous => ({ ...previous, [request.id]: result }));
    } catch (cause) { failed(cause); }
    finally { pendingRef.current = false; if (mounted.current) setPending(''); }
  }

  async function refresh() {
    setError('');
    await loadList(tab);
    if (tab === 'conversations') setMessageAttempt(value => value + 1);
    if (selectedRequest && tab !== 'conversations') await loadRequest(selectedRequest.id);
  }

  async function mutate<T>(action: string, path: string, body: unknown, completed: (result: T) => void | Promise<void>, revision?: number) {
    if (pendingRef.current || expired || renewing) return;
    const safetyAction = path === '/api/reports' || path === '/api/blocks' || /\/(cancel|withdraw|close)$/.test(path);
    if (!rulesAccepted && !safetyAction) { setError('Сначала примите правила сообщества. Набранный текст остался на экране.'); return; }
    pendingRef.current = true; setPending(action); setError(''); setNotice('');
    try {
      const id = revision === undefined ? keys.current.key(action, body) : undefined;
      const result = await workflowWrite<T>(path, sessionRef.current.token, body, id, revision);
      keys.current.clear(action);
      if (mounted.current) await completed(result);
    } catch (cause) {
      if (mounted.current && revision !== undefined && cause instanceof ApiError && (cause.status === 409 || cause.status === 428)) {
        setDraftConflict({ id: path.split('/').at(-1)! });
        setError('Черновик изменился на сервере. Ваш текст остался в редакторе. Загрузите актуальную версию для сравнения перед сохранением.');
      } else failed(cause);
    }
    finally { pendingRef.current = false; if (mounted.current) setPending(''); }
  }

  async function loadLatestDraft() {
    if (!draftConflict || pendingRef.current || expired || renewing) return;
    const id = draftConflict.id;
    pendingRef.current = true; setPending('reload-draft'); setError('');
    try {
      const latest = await workflowRead<HelpRequest>(`/api/requests/${id}`, sessionRef.current.token);
      if (mounted.current) setDraftConflict(current => current?.id === id ? { id, latest } : current);
    } catch (cause) { failed(cause); }
    finally { pendingRef.current = false; if (mounted.current) setPending(''); }
  }

  function resolveDraftConflict(mode: 'server' | 'mine' | 'copy') {
    if (!editor || !draftConflict?.latest || busy) return;
    const latest = draftConflict.latest;
    if (mode === 'server' && !window.confirm('Заменить набранный текст версией с сервера? Несохранённые изменения этого вопроса будут потеряны.')) return;
    if (mode !== 'copy' && latest.status !== 'draft') return;
    setEditor(mode === 'copy'
      ? { draft: { ...editor.draft, taxonomyVersion: taxonomy.version } }
      : { draft: mode === 'server' ? requestInput(latest) : editor.draft, original: latest });
    setDraftConflict(null); setError('');
    setNotice(mode === 'copy' ? 'Текст перенесён в новый вопрос. Сохранение и публикация будут отдельными действиями.' : mode === 'mine' ? 'Ваш текст сохранён в редакторе. При следующем сохранении он заменит актуальный черновик на сервере; проверьте все поля.' : 'Загружена актуальная версия черновика.');
  }

  async function renew() {
    if (renewing || pendingRef.current) return;
    setRenewing(true); setError('');
    try { await onRenewSession(); setExpired(false); setNotice('Вход восстановлен. Проверьте текст и повторите нужное действие.'); }
    catch (cause) { setError(renewalErrorMessage(cause)); }
    finally { setRenewing(false); }
  }

  function beginEditor(request?: HelpRequest) {
    if (writeBlocked) return;
    if (editor && JSON.stringify(editor.draft) !== JSON.stringify(editor.original ? requestInput(editor.original) : newRequest(taxonomy.version)) && !window.confirm('Открыть другой черновик? Несохранённые изменения текущего вопроса будут потеряны.')) return;
    keys.current.clear('create-request');
    setEditor({ draft: request ? requestInput(request) : newRequest(taxonomy.version), ...(request ? { original: request } : {}) });
    setTab('mine'); setDraftConflict(null); setError(''); setNotice('');
  }
  function copyRequest(request: HelpRequest) {
    if (writeBlocked || request.authorId !== session.user.id || !['expired', 'cancelled', 'resolved', 'closed_unresolved'].includes(request.status)) return;
    if (editorDirty && !window.confirm('Создать новый вопрос на основе этого? Несохранённые изменения открытого черновика будут потеряны.')) return;
    keys.current.clear('create-request');
    setEditor({ draft: { ...requestInput(request), taxonomyVersion: taxonomy.version } }); setTab('mine'); setDraftConflict(null); setError('');
    setNotice('Поля перенесены в новый вопрос. Проверьте, что изменилось, затем отдельно сохраните и опубликуйте. История исходного вопроса останется на месте.');
  }
  function cancelEditor() {
    if (editor && JSON.stringify(editor.draft) !== JSON.stringify(editor.original ? requestInput(editor.original) : newRequest(taxonomy.version)) && !window.confirm('Закрыть редактор? Несохранённые изменения этого вопроса будут потеряны.')) return;
    setEditor(null); setDraftConflict(null); keys.current.clear('create-request');
  }
  async function saveRequest() {
    if (!editor || draftConflict) return;
    const original = editor.original;
    const body = { ...editor.draft, title: editor.draft.title.trim(), body: editor.draft.body.trim(), attempt: editor.draft.attempt?.trim() ?? '' };
    await mutate<HelpRequest>(original ? `edit-${original.id}` : 'create-request', original ? `/api/requests/${original.id}` : '/api/requests', body, async request => {
      setEditor(null); setDraftConflict(null); setSelectedRequest(request); setOffers([]); setNotice('Черновик сохранён. Проверьте вопрос и опубликуйте, когда будете готовы.'); await loadList('mine');
    }, original?.revision);
  }
  async function requestAction(request: HelpRequest, action: 'publish' | 'cancel') {
    if (action === 'publish' && (!catalog.byId.has(request.topicId) || request.taxonomyVersion !== taxonomy.version)) { setError('Выберите для черновика тему в образовательных траекториях или карьере, затем сохраните его.'); return; }
    if (action === 'publish' && editor?.original?.id === request.id && editorDirty) { setError('Сначала сохраните изменения черновика, чтобы опубликовать актуальный текст.'); return; }
    if (action === 'cancel' && !window.confirm('Отменить этот вопрос? Новые отклики больше не будут приниматься.')) return;
    await mutate<HelpRequest>(`${action}-${request.id}`, `/api/requests/${request.id}/${action}`, {}, async next => {
      setSelectedRequest(next); setNotice(action === 'publish' ? 'Вопрос опубликован. Он появится у участников с подходящими знаниями.' : 'Вопрос отменён.'); await loadList('mine'); await loadRequest(next.id);
    });
  }
  async function sendOffer(request: HelpRequest) {
    const message = (offerDrafts[request.id] ?? '').trim();
    if (!message) { setError('Напишите, чем можете помочь.'); return; }
    await mutate<Offer>(`offer-${request.id}`, `/api/requests/${request.id}/offers`, { message }, async () => {
      setOfferDrafts(previous => ({ ...previous, [request.id]: '' })); setNotice('Отклик отправлен. Автор выберет помощника; диалог появится в разделе «Диалоги».'); await loadRequest(request.id); await loadList('feed');
    });
  }
  async function withdraw(offer: Offer) {
    if (!window.confirm('Отозвать отклик? Автор больше не сможет выбрать его.')) return;
    await mutate<Offer>(`withdraw-${offer.id}`, `/api/offers/${offer.id}/withdraw`, {}, async () => { setNotice('Отклик отозван.'); await loadRequest(offer.requestId); await loadList(tab); });
  }
  async function accept(offer: Offer) {
    await mutate<Conversation>(`accept-${offer.id}`, `/api/offers/${offer.id}/accept`, {}, async conversation => {
      setConversations(previous => [conversation, ...previous.filter(item => item.id !== conversation.id)]); setSelectedConversationId(conversation.id); setTab('conversations'); setNotice('Помощник выбран. Теперь можно обсудить вопрос вдвоём.'); await loadList('conversations');
    });
  }
  async function openConversation(id: string) { setSelectedConversationId(id); setTab('conversations'); await loadList('conversations'); }

  useEffect(() => {
    const conversationId = selectedConversationId;
    if (!conversationId || tab !== 'conversations' || expired) return;
    if (!messagesReady[conversationId]) setMessageLoading(conversationId);
    stickToBottom.current = true; setNewMessagesBelow(false);
    let stopped = false;
    let running = false;
    let timer: number | undefined;
    async function poll() {
      if (stopped || running || !shouldPoll(tab, conversationId, document.hidden, expired)) return;
      running = true;
      try {
        const data = await workflowRead<MessagePage>(`/api/conversations/${conversationId}/messages?after=${cursor.current[conversationId!] ?? 0}&limit=50`, sessionRef.current.token);
        if (stopped) return;
        cursor.current[conversationId!] = data.nextAfter;
        setMessagesReady(previous => ({ ...previous, [conversationId!]: true }));
        setMessageLoading('');
        setMessages(previous => ({ ...previous, [conversationId!]: mergeMessages(previous[conversationId!] ?? [], data.items) }));
        const list = await workflowRead<{ items: Conversation[] }>('/api/conversations', sessionRef.current.token);
        if (!stopped) { setConversations(list.items); setLastRefresh(previous => ({ ...previous, [conversationId!]: new Date().toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit', second: '2-digit' }) })); }
      } catch (cause) { if (!stopped) failed(cause); }
      finally { running = false; if (!stopped) setMessageLoading(''); if (!stopped && !document.hidden) timer = window.setTimeout(() => void poll(), 5000); }
    }
    const visibility = () => { if (timer) window.clearTimeout(timer); if (!document.hidden) void poll(); };
    document.addEventListener('visibilitychange', visibility);
    void poll();
    return () => { stopped = true; if (timer) window.clearTimeout(timer); document.removeEventListener('visibilitychange', visibility); };
  }, [selectedConversationId, tab, session.token, expired, failed, messageAttempt]);

  const currentMessageCount = selectedConversationId ? messages[selectedConversationId]?.length ?? 0 : 0;
  useEffect(() => {
    const list = messageList.current;
    if (!list || tab !== 'conversations') return;
    if (stickToBottom.current) { list.scrollTop = list.scrollHeight; setNewMessagesBelow(false); }
    else setNewMessagesBelow(true);
  }, [currentMessageCount, selectedConversationId, tab]);

  async function sendMessage(event: FormEvent) {
    event.preventDefault();
    const conversation = selectedConversation;
    if (!conversation || conversation.status !== 'active' || pendingRef.current || writeBlocked) return;
    const text = (chatDrafts[conversation.id] ?? '').trim();
    if (!text) return;
    const action = `message-${conversation.id}`;
    const clientMessageId = keys.current.key(action, text);
    pendingRef.current = true; setPending(action); setError(''); setNotice('');
    try {
      const message = await workflowWrite<Message>(`/api/conversations/${conversation.id}/messages`, sessionRef.current.token, { clientMessageId, text });
      keys.current.clear(action);
      if (!mounted.current) return;
      setMessages(previous => ({ ...previous, [conversation.id]: mergeMessages(previous[conversation.id] ?? [], [message]) }));
      // Do not advance the polling cursor here: another message may precede this one.
      stickToBottom.current = true;
      setChatDrafts(previous => ({ ...previous, [conversation.id]: '' }));
    } catch (cause) { failed(cause); }
    finally { pendingRef.current = false; if (mounted.current) setPending(''); }
  }
  async function closeConversation(event: FormEvent) {
    event.preventDefault();
    if (!closeForm || !window.confirm('Завершить диалог? После этого новые сообщения отправить не получится.')) return;
    const form = closeForm;
    await mutate<Conversation>(`close-${form.conversationId}`, `/api/conversations/${form.conversationId}/close`, { outcome: form.outcome, comment: form.comment.trim(), ...(conversations.find(item => item.id === form.conversationId)?.authorId === session.user.id ? { nextStep: form.nextStep.trim() } : {}) }, conversation => {
      setConversations(previous => previous.map(item => item.id === conversation.id ? conversation : item)); setCloseForm(null); setNotice('Диалог завершён. Результат сохранён.');
    });
  }
  async function report(event: FormEvent) {
    event.preventDefault();
    if (!reportForm) return;
    const form = reportForm;
    await mutate<{ id: string; status: string }>(`report-${form.conversationId}`, '/api/reports', { conversationId: form.conversationId, category: form.category, text: form.text.trim() }, () => {
      setReportForm(null); setNotice('Жалоба передана в очередь модерации. Срок рассмотрения пока не установлен. Чтобы сразу прекратить общение, заблокируйте участника.');
    });
  }
  async function block(conversation: Conversation) {
    if (!window.confirm(`Заблокировать участника «${conversation.otherDisplayName}»? Общие активные диалоги завершатся; новые отклики и сообщения между вами будут недоступны. Неотправленные тексты сообщений будут удалены.`)) return;
    await mutate<{ ok: boolean }>(`block-${conversation.otherUserId}`, '/api/blocks', { userId: conversation.otherUserId }, async () => {
      setSelectedConversationId(null); setSelectedRequest(null); setCloseForm(null); setReportForm(null);
      const paired = conversations.filter(item => item.otherUserId === conversation.otherUserId).map(item => item.id);
      setChatDrafts(previous => Object.fromEntries(Object.entries(previous).filter(([id]) => !paired.includes(id))));
      setNotice('Участник заблокирован. Общение между вами прекращено.'); await loadList('conversations');
    });
  }

  function facetLabels(request: HelpRequest) {
    return Object.entries(request.facets).map(([id, values]) => {
      const facet = taxonomy.facets.find(item => item.id === id);
      return `${facet?.label ?? id}: ${values.map(value => facet?.values.find(item => item.id === value)?.label ?? value).join(', ')}${request.requiredFacets.includes(id) ? ' · обязательно' : ''}`;
    });
  }

  return <section id="workspace" className="workflow-section" aria-labelledby="workspace-title">
    <div className="section-heading"><div><span className="eyebrow">ВОПРОСЫ СОЕДИНЯЮТ ЗНАНИЯ</span><h2 id="workspace-title">Учиться и помогать</h2></div><div className="workflow-actions"><button className="button primary small" disabled={writeBlocked} onClick={() => beginEditor()}>Задать вопрос +</button><button className="button secondary small" disabled={busy} onClick={() => { setTab('feed'); setError(''); }}>Помочь с вопросом</button></div></div>
    <p className="section-intro">Опишите задачу, выберите собеседника из откликнувшихся и сохраните, что удалось понять. В разделе «Могу помочь» можно поделиться своими знаниями.</p>
    {!rulesAccepted && <p className="notice workflow-notice">Чтобы задавать вопросы, откликаться и отправлять сообщения, <a className="text-button" href="#community-rules">примите правила сообщества</a>. Историю можно читать; жалобы, блокировка и завершение общения доступны.</p>}
    <div className="workspace-nav" role="tablist" aria-label="Вопросы и общение">{tabs.map(([value, label]) => <button id={`tab-${value}`} key={value} type="button" role="tab" aria-selected={tab === value} aria-controls={`panel-${value}`} tabIndex={tab === value ? 0 : -1} onKeyDown={event => { const position = tabs.findIndex(([id]) => id === value); const next = event.key === 'ArrowRight' ? (position + 1) % tabs.length : event.key === 'ArrowLeft' ? (position + tabs.length - 1) % tabs.length : event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : -1; if (next < 0) return; event.preventDefault(); setTab(tabs[next][0]); setNotice(''); setError(''); document.getElementById(`tab-${tabs[next][0]}`)?.focus(); }} onClick={() => { setTab(value); setNotice(''); setError(''); }} className={tab === value ? 'selected' : ''}>{label}</button>)}</div>
    {expired && <div className="notice notice-error" role="alert"><span>Время входа истекло. Черновики остаются на экране.</span><button className="button secondary small" disabled={renewing} onClick={() => void renew()}>{renewing ? 'Входим…' : 'Войти снова'}</button></div>}
    {error && <div className="notice notice-error" role="alert"><span>{error}</span>{!expired && <button className="text-button" disabled={!!pending || loading} onClick={() => void refresh()}>Обновить данные</button>}</div>}
    {notice && <p className="notice workflow-notice" role="status">{notice}</p>}
    <div className="workflow-toolbar"><span role="status">{loading ? 'Обновляем список…' : 'Данные сохраняются в вашем профиле сообщества'}</span><button className="text-button" disabled={loading || busy} onClick={() => void refresh()}>Обновить</button></div>

    {detailLoading && <p className="workflow-loading" role="status">Загружаем вопрос и отклики…</p>}
    <div id="panel-mine" role="tabpanel" tabIndex={0} aria-labelledby="tab-mine" hidden={tab !== 'mine'}>
      {editor && draftConflict && <div className="draft-conflict" role="alert">
        <h3>Сначала сравните версии вопроса</h3><p>Набранные вами поля остаются в редакторе ниже. Получение версии с сервера их не заменяет.</p>
        <button type="button" className="button secondary small" disabled={busy} onClick={() => void loadLatestDraft()}>{pending === 'reload-draft' ? 'Загружаем…' : draftConflict.latest ? 'Обновить версию с сервера' : 'Получить актуальную версию'}</button>
        {draftConflict.latest && <><details className="server-draft"><summary>Версия с сервера · {statusLabels[draftConflict.latest.status]}</summary><h4>{draftConflict.latest.title}</h4><p className="plain-text">{draftConflict.latest.body}</p>{draftConflict.latest.attempt && <p className="plain-text">Что уже пробовали: {draftConflict.latest.attempt}</p>}<p>Тема: {topicLabel(draftConflict.latest.topicId)}</p><p>Цель: {goalLabels[draftConflict.latest.learningGoal]}</p><p>Опыт: {draftConflict.latest.desiredExperience ? experienceLabels[draftConflict.latest.desiredExperience] : 'Любой подходящий'}</p>{facetLabels(draftConflict.latest).map(label => <p key={label}>{label}</p>)}</details><div className="workflow-actions">{draftConflict.latest.status === 'draft' ? <><button className="button secondary small" disabled={busy} onClick={() => resolveDraftConflict('mine')}>Оставить мой текст для сохранения</button><button className="button quiet small" disabled={busy} onClick={() => resolveDraftConflict('server')}>Загрузить версию с сервера</button></> : <><p>Этот вопрос уже нельзя редактировать как черновик. Вы можете перенести свой текст в новый вопрос.</p><button className="button secondary small" disabled={busy} onClick={() => resolveDraftConflict('copy')}>Перенести текст в новый вопрос</button></>}</div></>}
      </div>}
      {editor && <RequestEditor editor={editor} taxonomy={taxonomy} busy={busy} writesAllowed={rulesAccepted} saveBlocked={!!draftConflict || !rulesAccepted} onChange={draft => { setEditor({ ...editor, draft }); keys.current.clear('create-request'); }} onSave={() => void saveRequest()} onCancel={cancelEditor} />}
      {!pages.mine.items.length && listReady.mine && !loading && !error && <div className="empty-state"><h3>Начните с того, что хочется понять</h3><p>Сохраните вопрос как черновик, уточните тему и опубликуйте его для людей с подходящим опытом.</p></div>}
      <div className="request-list">{pages.mine.items.map(request => <article className={`request-card ${selectedRequest?.id === request.id ? 'is-selected' : ''}`} key={request.id}><div className="request-card-top"><span className={`status-badge status-${request.status}`}>{statusLabels[request.status]}</span><time dateTime={request.createdAt}>{time(request.createdAt)}</time></div><p className="request-topic">{topicLabel(request.topicId)}</p><h3><button className="request-link" onClick={() => void loadRequest(request.id)} disabled={busy}>{request.title}</button></h3><p className="request-excerpt">{request.body}</p><button className="text-button" onClick={() => void loadRequest(request.id)} disabled={busy}>Открыть вопрос и отклики →</button></article>)}</div>
      {pages.mine.nextCursor && <button className="button secondary small" disabled={loading || busy} onClick={() => void loadList('mine', pages.mine.nextCursor!)}>Ещё вопросы</button>}
    </div>

    <div id="panel-feed" role="tabpanel" tabIndex={0} aria-labelledby="tab-feed" hidden={tab !== 'feed'}>
      {(!profile.availableToHelp || !profile.competencies.length) && <p className="notice workflow-notice">Добавьте знания в профиль и включите «Готов(а) помогать», чтобы получать подходящие вопросы. <a className="text-button" href="#knowledge">К профилю</a></p>}
      {!pages.feed.items.length && listReady.feed && !loading && !error && <div className="empty-state"><h3>Подходящих вопросов пока нет</h3><p>Здесь появятся вопросы по вашим сохранённым знаниям. Учитываются тема, обязательные условия и доступность помощника.</p><a className="text-button" href="#knowledge">Уточнить свои знания</a></div>}
      <div className="request-list">{pages.feed.items.map(request => <article className={`request-card ${selectedRequest?.id === request.id ? 'is-selected' : ''}`} key={request.id}><div className="request-card-top"><span>{request.authorName}</span><time dateTime={request.createdAt}>{time(request.createdAt)}</time></div><p className="request-topic">{topicLabel(request.topicId)}</p><h3><button className="request-link" onClick={() => void loadRequest(request.id)} disabled={busy}>{request.title}</button></h3><p className="request-excerpt">{request.body}</p>{request.match && <><p className="match-reason">По вашему знанию: {topicLabel(request.match.topicId)}.{!request.match.explanation && (request.match.narrower ? ' Ваш опыт относится к части этой темы - обозначьте границы помощи.' : ' Тема соответствует вашему профилю.')}</p><MatchReasons explanation={request.match.explanation} taxonomy={taxonomy} /></>}{request.myOffer && <span className="status-badge">{offerLabels[request.myOffer.status]}</span>}<button className="text-button" onClick={() => void loadRequest(request.id)} disabled={busy}>Посмотреть и откликнуться →</button></article>)}</div>
      {pages.feed.nextCursor && <button className="button secondary small" disabled={loading || busy} onClick={() => void loadList('feed', pages.feed.nextCursor!)}>Ещё вопросы</button>}
    </div>

    {selectedRequest && <article className="request-detail" hidden={tab === 'conversations' || (tab === 'mine' ? selectedRequest.authorId !== session.user.id : selectedRequest.authorId === session.user.id)} aria-label="Подробности вопроса">
      <div className="section-heading compact"><span className={`status-badge status-${selectedRequest.status}`}>{statusLabels[selectedRequest.status]}</span><button type="button" className="icon-button" aria-label="Закрыть подробности вопроса" onClick={() => { ++detailGeneration.current; setDetailLoading(''); setSelectedRequest(null); }} disabled={!!pending}>×</button></div>
      {selectedRequest.status === 'draft' && catalog.byId.has(selectedRequest.topicId) && selectedRequest.taxonomyVersion !== taxonomy.version && <div className="archived-topic-notice" role="status"><strong>Каталог обновился</strong><p>Откройте «Изменить», подтвердите тему и сохраните черновик перед публикацией. Ваш текст сохранён.</p></div>}
      {!catalog.byId.has(selectedRequest.topicId) && <div className="archived-topic-notice"><strong>Вопрос из сохранённого направления</strong><p>{selectedRequest.status === 'draft' ? 'Чтобы опубликовать черновик, измените его и выберите тему в образовании или карьере. Текст и история сохранены.' : 'Сейчас подбор новых участников работает только в образовании и карьере. История этого вопроса и существующий диалог сохранены.'}</p></div>}
      <p className="request-topic">{topicLabel(selectedRequest.topicId)}</p><h3 ref={requestHeading} tabIndex={-1}>{selectedRequest.title}</h3><p className="workflow-byline">{selectedRequest.authorName} · {goalLabels[selectedRequest.learningGoal]}</p><p className="plain-text">{selectedRequest.body}</p>
      {selectedRequest.attempt && <div className="question-context"><h4>Что уже пробовали</h4><p className="plain-text">{selectedRequest.attempt}</p></div>}
      {selectedRequest.desiredExperience && <p className="workflow-hint">Желаемый опыт: {experienceLabels[selectedRequest.desiredExperience]}</p>}
      {!!Object.keys(selectedRequest.facets).length && <ul className="request-facets">{facetLabels(selectedRequest).map(label => <li key={label}>{label}</li>)}</ul>}
      {selectedRequest.authorId !== session.user.id && <MatchReasons explanation={selectedRequest.match?.explanation} taxonomy={taxonomy} />}
      {selectedRequest.authorId === session.user.id ? <>
        {catalog.byId.has(selectedRequest.topicId) && ['draft', 'open'].includes(selectedRequest.status) && <RequestReachPanel request={selectedRequest} reach={reachResults[selectedRequest.id]} hasUnsavedChanges={editor?.original?.id === selectedRequest.id && editorDirty} busy={busy} checking={pending === `reach-${selectedRequest.id}`} parentLabel={topicLabel(reachResults[selectedRequest.id]?.parentTopicId ?? '')} onCheck={() => void checkReach(selectedRequest)} onEdit={() => beginEditor(selectedRequest)} />}
        <div className="workflow-actions">{selectedRequest.status === 'draft' && <><button className="button primary" disabled={writeBlocked || !catalog.byId.has(selectedRequest.topicId) || selectedRequest.taxonomyVersion !== taxonomy.version} onClick={() => void requestAction(selectedRequest, 'publish')}>Опубликовать вопрос</button><button className="button secondary" disabled={writeBlocked} onClick={() => beginEditor(selectedRequest)}>Изменить</button></>}{['draft', 'open'].includes(selectedRequest.status) && <button className="button quiet" disabled={busy} onClick={() => void requestAction(selectedRequest, 'cancel')}>Отменить вопрос</button>}{selectedRequest.conversationId && <button className="button primary" disabled={busy} onClick={() => void openConversation(selectedRequest.conversationId!)}>Перейти к диалогу</button>}{['expired', 'cancelled', 'resolved', 'closed_unresolved'].includes(selectedRequest.status) && <button className="button secondary" disabled={writeBlocked} onClick={() => copyRequest(selectedRequest)}>Создать новый на основе вопроса</button>}</div>
        {selectedRequest.status !== 'draft' && <div className="offer-list"><h4>Отклики</h4>{!offers.length && <p className="workflow-hint">{selectedRequest.status === 'open' ? 'Откликов пока нет. Вопрос показывается участникам, чьи знания подходят к теме и условиям.' : 'По этому вопросу откликов нет.'}</p>}{offers.map(offer => <article className="offer-card" key={offer.id}><div className="request-card-top"><strong>{offer.helperName}</strong><span className="status-badge">{offerLabels[offer.status]}</span></div><p className="plain-text">{offer.message}</p><div className="helper-knowledge"><p>{topicLabel(offer.competency.topicId)}</p><p>{experienceLabels[offer.competency.experienceKind]} · Знания указаны участником</p>{offer.competency.description && <p className="plain-text">{offer.competency.description}</p>}{offer.narrower && !offer.explanation && <p>Опыт относится к части вашей темы. Уточните, какая помощь возможна.</p>}</div><MatchReasons explanation={offer.explanation} taxonomy={taxonomy} />{offer.status === 'pending' && selectedRequest.status === 'open' && catalog.byId.has(selectedRequest.topicId) && <button className="button primary small" disabled={writeBlocked} onClick={() => void accept(offer)}>Выбрать помощника</button>}</article>)}</div>}
      </> : <div className="offer-composer">
        {offers.map(offer => <div className="my-offer" key={offer.id}><span className="status-badge">{offerLabels[offer.status]}</span><p className="plain-text">{offer.message}</p>{offer.status === 'pending' && <button className="text-button" disabled={busy} onClick={() => void withdraw(offer)}>Отозвать отклик</button>}</div>)}
        {selectedRequest.conversationId && <button className="button primary" disabled={busy} onClick={() => void openConversation(selectedRequest.conversationId!)}>Перейти к диалогу</button>}
        {selectedRequest.status === 'open' && catalog.byId.has(selectedRequest.topicId) && !offers.length && <form onSubmit={event => { event.preventDefault(); void sendOffer(selectedRequest); }}><fieldset className="workflow-fields" disabled={writeBlocked}><label className="field">Чем можете помочь<textarea rows={3} value={offerDrafts[selectedRequest.id] ?? ''} maxLength={1000} required onChange={event => { setOfferDrafts(previous => ({ ...previous, [selectedRequest.id]: event.target.value })); keys.current.clear(`offer-${selectedRequest.id}`); }} placeholder="Коротко опишите, какую часть вопроса можете объяснить." /></label><p className="workflow-hint">Автор увидит ваш отклик и описание подходящего знания. Общение начнётся, когда он выберет вас.</p><button type="submit" className="button primary">{pending === `offer-${selectedRequest.id}` ? 'Отправляем…' : 'Могу помочь'}</button></fieldset></form>}
      </div>}
    </article>}

    <div id="panel-conversations" role="tabpanel" tabIndex={0} aria-labelledby="tab-conversations" hidden={tab !== 'conversations'}>
      {!conversations.length && listReady.conversations && !loading && !error && <div className="empty-state"><h3>Здесь начнётся разговор</h3><p>Диалог появится, когда автор вопроса выберет помощника из откликнувшихся участников.</p></div>}
      <div className="conversation-layout"><div className="conversation-list" aria-label="Список диалогов">{conversations.map(conversation => <button className={`conversation-item ${selectedConversationId === conversation.id ? 'selected' : ''}`} key={conversation.id} aria-pressed={selectedConversationId === conversation.id} onClick={() => { setSelectedConversationId(conversation.id); setError(''); }}><strong>{conversation.otherDisplayName}</strong><span>{conversation.title}</span><small>{conversation.status === 'active' ? 'Можно написать' : 'Завершён'}</small></button>)}</div>
      {!selectedConversation && !!conversations.length && <p className="empty-state">Выберите диалог, чтобы прочитать сообщения.</p>}
      {selectedConversation && <article className="chat-panel" aria-label="Переписка по вопросу"><div className="chat-heading"><h3>{selectedConversation.title}</h3><p>Собеседник: {selectedConversation.otherDisplayName}</p><small>{selectedConversation.status === 'active' ? 'Сообщения видны двум участникам диалога' : 'Диалог завершён · доступно чтение'}</small></div>
        <ol ref={messageList} className="message-list" aria-label="Сообщения" tabIndex={0} aria-busy={messageLoading === selectedConversation.id} onScroll={event => { const list = event.currentTarget; stickToBottom.current = list.scrollHeight - list.scrollTop - list.clientHeight < 70; if (stickToBottom.current) setNewMessagesBelow(false); }}>{(messages[selectedConversation.id] ?? []).map(message => <li className={message.senderId === session.user.id ? 'message own' : 'message'} key={message.id}><div><strong>{message.senderId === session.user.id ? 'Вы' : message.senderName}</strong><time dateTime={message.createdAt}>{time(message.createdAt)}</time></div><p className="plain-text">{message.text}</p></li>)}</ol>
        {messageLoading === selectedConversation.id && <p className="workflow-hint chat-empty" role="status">Загружаем сообщения…</p>}
        {messagesReady[selectedConversation.id] && !(messages[selectedConversation.id]?.length) && <p className="workflow-hint chat-empty">{selectedConversation.status === 'active' ? 'Сообщений пока нет. Поздоровайтесь и уточните вопрос.' : 'В этом диалоге нет сообщений.'}</p>}
        <p className="chat-update">{lastRefresh[selectedConversation.id] ? `Проверено в ${lastRefresh[selectedConversation.id]}. ` : ''}Новые сообщения проверяются каждые 5 секунд, пока этот раздел открыт.</p>
        {newMessagesBelow && <button type="button" className="text-button chat-jump" onClick={() => { const list = messageList.current; if (list) list.scrollTop = list.scrollHeight; stickToBottom.current = true; setNewMessagesBelow(false); }}>К новым сообщениям ↓</button>}
        {selectedConversation.status === 'active' ? <form onSubmit={event => void sendMessage(event)} className="chat-composer"><fieldset className="workflow-fields" disabled={writeBlocked}><label className="field">Ваше сообщение<textarea rows={3} maxLength={2000} required value={chatDrafts[selectedConversation.id] ?? ''} onChange={event => { setChatDrafts(previous => ({ ...previous, [selectedConversation.id]: event.target.value })); keys.current.clear(`message-${selectedConversation.id}`); }} placeholder="Обсудите вопрос. Не отправляйте пароли, документы и личные контакты." /></label><div className="workflow-actions"><small>{[...(chatDrafts[selectedConversation.id] ?? '')].length} / 2000</small><button className="button primary" type="submit" disabled={!(chatDrafts[selectedConversation.id] ?? '').trim()}>{pending === `message-${selectedConversation.id}` ? 'Отправляем…' : 'Отправить'}</button></div></fieldset></form> : <div className="conversation-result"><h4>{selectedConversation.moderationClosed ? 'Диалог закрыт модератором' : selectedConversation.outcome ? outcomeLabels[selectedConversation.outcome] : 'Общение завершено'}</h4>{selectedConversation.moderationClosed && <p>Учебный результат не оценивался.</p>}{selectedConversation.comment && <><h5>Итог разговора</h5><p className="plain-text">{selectedConversation.comment}</p></>}{selectedConversation.nextStep && <><h5>Следующий шаг автора вопроса</h5><p className="plain-text">{selectedConversation.nextStep}</p></>}{closeForm?.conversationId === selectedConversation.id && (closeForm.comment || closeForm.nextStep) && <details className="unsent-outcome"><summary>Неотправленный итог сохранён на экране</summary><p className="workflow-hint">Диалог уже завершён. Этот текст не был отправлен; его можно скопировать.</p>{closeForm.comment && <p className="plain-text">{closeForm.comment}</p>}{closeForm.nextStep && <><h5>Мой следующий шаг</h5><p className="plain-text">{closeForm.nextStep}</p></>}<button type="button" className="text-button" disabled={busy} onClick={() => { if (window.confirm('Удалить неотправленный итог и следующий шаг?')) setCloseForm(null); }}>Удалить черновик итога</button></details>}{chatDrafts[selectedConversation.id] && <details><summary>Неотправленный текст сохранён</summary><p className="plain-text">{chatDrafts[selectedConversation.id]}</p><button className="text-button" onClick={() => { if (window.confirm('Удалить неотправленный текст?')) setChatDrafts(previous => ({ ...previous, [selectedConversation.id]: '' })); }}>Удалить черновик сообщения</button></details>}</div>}
        <div className="chat-actions">{selectedConversation.status === 'active' && <button className="text-button" disabled={busy} onClick={() => { if (closeForm?.conversationId === selectedConversation.id) return; if ((closeForm?.comment || closeForm?.nextStep) && !window.confirm('Открыть итог другого диалога? Несохранённый итог и следующий шаг будут потеряны.')) return; setCloseForm({ conversationId: selectedConversation.id, outcome: selectedConversation.authorId === session.user.id ? 'helpful' : 'no_result', comment: '', nextStep: '' }); }}>Завершить диалог</button>}<button className="text-button" disabled={busy} onClick={() => { if (reportForm?.conversationId === selectedConversation.id) return; if (reportForm?.text && !window.confirm('Открыть жалобу на другой диалог? Несохранённый текст будет потерян.')) return; setReportForm({ conversationId: selectedConversation.id, category: 'abuse', text: '' }); }}>Пожаловаться</button><button className="text-button danger" disabled={busy} onClick={() => void block(selectedConversation)}>Заблокировать</button></div>
        {closeForm?.conversationId === selectedConversation.id && selectedConversation.status === 'active' && <form className="workflow-subform" onSubmit={event => void closeConversation(event)}><fieldset className="workflow-fields" disabled={busy}><h4>Как прошёл разговор?</h4><label className="field">Результат<select value={closeForm.outcome} onChange={event => { setCloseForm({ ...closeForm, outcome: event.target.value as Outcome }); keys.current.clear(`close-${selectedConversation.id}`); }}>{Object.entries(outcomeLabels).filter(([value]) => selectedConversation.authorId === session.user.id || value === 'no_result').map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label><label className="field">{selectedConversation.authorId === session.user.id ? 'Что удалось понять или решить' : 'Короткий итог разговора'} <span className="optional">необязательно</span><textarea rows={2} maxLength={500} value={closeForm.comment} onChange={event => { setCloseForm({ ...closeForm, comment: event.target.value }); keys.current.clear(`close-${selectedConversation.id}`); }} placeholder={selectedConversation.authorId === session.user.id ? 'Сформулируйте своими словами, что стало понятнее.' : 'Коротко опишите, на чём остановились.'} /></label>{selectedConversation.authorId === session.user.id && <label className="field">Мой следующий шаг <span className="optional">необязательно</span><textarea rows={2} maxLength={500} value={closeForm.nextStep} onChange={event => { setCloseForm({ ...closeForm, nextStep: event.target.value }); keys.current.clear(`close-${selectedConversation.id}`); }} placeholder="Что попробуете сделать самостоятельно после разговора?" /></label>}<p className="workflow-hint">После завершения новые сообщения недоступны. Итог и следующий шаг видны собеседнику.</p><div className="workflow-actions"><button type="submit" className="button primary small">Сохранить итог и завершить</button><button type="button" className="button quiet small" onClick={() => { if ((!closeForm.comment && !closeForm.nextStep) || window.confirm('Закрыть форму без сохранения итога и следующего шага?')) setCloseForm(null); }}>Назад</button></div></fieldset></form>}
        {reportForm?.conversationId === selectedConversation.id && <form className="workflow-subform" onSubmit={event => void report(event)}><fieldset className="workflow-fields" disabled={busy}><h4>Сообщить о проблеме</h4><label className="field">Причина<select value={reportForm.category} onChange={event => { setReportForm({ ...reportForm, category: event.target.value as ReportCategory }); keys.current.clear(`report-${selectedConversation.id}`); }}><option value="abuse">Оскорбления или давление</option><option value="spam">Спам</option><option value="other">Другое</option></select></label><label className="field">Что произошло<textarea rows={3} maxLength={2000} required value={reportForm.text} onChange={event => { setReportForm({ ...reportForm, text: event.target.value }); keys.current.clear(`report-${selectedConversation.id}`); }} /></label><p className="workflow-hint">Собеседник не увидит текст жалобы. Для немедленного прекращения общения используйте блокировку.</p><div className="workflow-actions"><button type="submit" className="button primary small">Отправить жалобу</button><button type="button" className="button quiet small" onClick={() => { if (!reportForm.text || window.confirm('Закрыть форму? Текст жалобы будет потерян.')) setReportForm(null); }}>Отмена</button></div></fieldset></form>}
      </article>}
      </div>
    </div>
  </section>;
}
