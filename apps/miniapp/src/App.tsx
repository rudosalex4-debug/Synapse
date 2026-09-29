import { useEffect, useMemo, useRef, useState, type FormEvent } from 'react';
import { buildCatalogIndex } from './catalog-index';
import { Workspace } from './Workspace';
import { NotificationSettings } from './NotificationSettings';
import { CommunityRules } from './CommunityRules';
import { ModerationWorkspace } from './ModerationWorkspace';
import { TopicJourney } from './TopicJourney';
import { ContextFields } from './ContextFields';
import { api, ApiError, errorMessage, logoutSession, needsProfileReload, needsSignIn, saveProfile } from './api';
import { bridgeSummary, loadBridge, type BridgeSummary } from './bridge';
import { renewalErrorMessage, renewProfileSession, type LoginSource } from './session-renewal';
import { newCompetency, profileInput, selectTopic, topicPath, validEvidenceUrl } from './profile-model';
import type { Bootstrap, CompetencyInput, ExperienceKind, Profile, ProfileInput, Session, Taxonomy } from './types';

const experienceLabels: Record<ExperienceKind, string> = {
  self_study: 'Изучаю самостоятельно', practice: 'Применяю на практике', teaching: 'Объясняю и обучаю', participation: 'Участвовал(а) в проектах',
};

function Arrow({ reverse = false }: { reverse?: boolean }) {
  return <svg viewBox="0 0 24 24" width="22" height="22" fill="none" aria-hidden="true" style={reverse ? { transform: 'rotate(180deg)' } : undefined}><path d="M4 12h15m-6-6 6 6-6 6" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" /></svg>;
}

function ErrorNotice({ message, retry }: { message: string; retry?: () => void }) {
  return <div className="notice notice-error" role="alert"><span>{message}</span>{retry && <button className="text-button" onClick={retry}>Повторить</button>}</div>;
}

function Diagnostics({ bridge, bootstrap, authenticated, state }: { bridge: BridgeSummary; bootstrap?: Bootstrap; authenticated: boolean; state: string }) {
  return <details className="diagnostics"><summary>Техническая проверка подключения <span aria-hidden="true">＋</span></summary><div className="diagnostic-body"><p>Проверка для команды. Успешный вход в демо не подтверждает запуск в реальном клиенте MAX.</p><dl>
    <div><dt>MAX Bridge</dt><dd>{bridge.state === 'loaded' ? 'Библиотека загружена' : bridge.state === 'loading' ? 'Загрузка…' : 'Недоступен'}</dd></div>
    <div><dt>Клиент</dt><dd>{bridge.platform}</dd></div>
    <div><dt>Данные запуска</dt><dd>{bridge.hasLaunchData ? 'Есть; проверяются сервером' : 'Отсутствуют'}</dd></div>
    <div><dt>API</dt><dd>{bootstrap ? `Доступен · ${bootstrap.version}` : state === 'loading' ? 'Проверяется…' : 'Недоступен'}</dd></div>
    <div><dt>Режим сервера</dt><dd>{bootstrap ? bootstrap.mode === 'demo' ? 'Демонстрационный' : 'MAX' : 'Не определён'}</dd></div>
    <div><dt>MAX на сервере</dt><dd>{bootstrap?.maxConfigured ? 'Настроен' : 'Не настроен'}</dd></div>
    <div><dt>Сессия</dt><dd>{authenticated ? 'Подтверждена сервером' : 'Вход не выполнен'}</dd></div>
  </dl></div></details>;
}

function CompetencyEditor({ initial, taxonomy, onAdd, onCancel, editing }: { initial: CompetencyInput; taxonomy: Taxonomy; onAdd: (competency: CompetencyInput) => void; onCancel: () => void; editing: boolean }) {
  const [draft, setDraft] = useState(initial);
  const [error, setError] = useState('');
  const catalogIndex = useMemo(() => buildCatalogIndex(taxonomy), [taxonomy]);
  const path = catalogIndex.path(draft.topicId);
  const skill = path.find((topic) => topic.level === 2 && topic.active)?.id ?? '';
  function changeTopic(id: string) { setDraft((current) => selectTopic(current, id, taxonomy)); setError(''); }
  function submit(event: FormEvent) {
    event.preventDefault();
    if (!skill) { setError('Выберите направление и тему своего опыта.'); return; }
    if (!validEvidenceUrl(draft.evidenceUrl ?? '')) { setError('Для подтверждения нужна HTTPS-ссылка без логина и пароля в адресе.'); return; }
    onAdd({ ...draft, description: draft.description.trim(), ...(draft.evidenceUrl?.trim() ? { evidenceUrl: draft.evidenceUrl.trim() } : { evidenceUrl: undefined }) });
  }
  return <form className="competency-editor" onSubmit={submit}>
    <div className="section-heading compact"><div><span className="eyebrow">ТЕМА, В КОТОРОЙ ВЫ РАЗБИРАЕТЕСЬ</span><h3>{editing ? 'Уточнить знание' : 'Добавить знание'}</h3></div><button type="button" className="icon-button" aria-label="Закрыть добавление знания" onClick={onCancel}>×</button></div>
    <TopicJourney taxonomy={taxonomy} value={draft.topicId} onChange={changeTopic} purpose="competency" />
    {skill && <ContextFields taxonomy={taxonomy} topicId={draft.topicId} value={draft.facets} onChange={(id, values) => setDraft(current => { const facets = { ...current.facets }; if (values.length) facets[id] = values; else delete facets[id]; return { ...current, facets }; })} />}
    <div className="field-grid"><label className="field">Ваш опыт<select value={draft.experienceKind} onChange={(event) => setDraft({ ...draft, experienceKind: event.target.value as ExperienceKind })}>{Object.entries(experienceLabels).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label></div>
    <label className="field">Что можете объяснить<textarea rows={3} value={draft.description} maxLength={500} placeholder="Например: прошёл отбор на стажировку, расскажу об этапах и помогу подготовиться к собеседованию." onChange={(event) => setDraft({ ...draft, description: event.target.value })} /><small>Коротко опишите опыт и границы темы. Не добавляйте личные контакты.</small></label>
    {skill && <details className="extra-options"><summary>Добавить подтверждение опыта</summary><div className="extra-options-body">
      <label className="field">Ссылка на подтверждение <span className="optional">необязательно</span><input type="url" value={draft.evidenceUrl ?? ''} maxLength={2048} placeholder="https://…" onChange={(event) => setDraft({ ...draft, evidenceUrl: event.target.value })} /><small>Диплом, сертификат, профиль участника или пример работы. Ссылка сохраняется со статусом «не проверено».</small></label>
      <label className="field">Кому доступна ссылка<select value={draft.evidenceVisibility} onChange={(event) => setDraft({ ...draft, evidenceVisibility: event.target.value as 'private' | 'participants' })}><option value="private">Только мне</option><option value="participants">Тем, кому доступна моя карточка</option></select></label>
    </div></details>}
    {error && <ErrorNotice message={error} />}
    <div className="editor-actions"><button className="button primary" type="submit">{editing ? 'Применить к профилю' : 'Добавить в профиль'} <span aria-hidden="true">＋</span></button><button className="button quiet" type="button" onClick={onCancel}>Отмена</button></div><p className="field-hint">После добавления сохраните профиль целиком.</p>
  </form>;
}

function ProfileForm({ profile, taxonomy, session, rulesAccepted, onRulesRequired, onSaved, onLogout, onRenewSession, onSessionExpired, onDirtyChange }: { profile: Profile; taxonomy: Taxonomy; session: Session; rulesAccepted: boolean; onRulesRequired: () => void; onSaved: (profile: Profile) => void; onLogout: () => Promise<void>; onRenewSession: () => Promise<void>; onSessionExpired: () => void; onDirtyChange: (dirty: boolean) => void }) {
  const [draft, setDraft] = useState<ProfileInput>(() => profileInput(profile));
  const [editor, setEditor] = useState<{ index: number | null; competency: CompetencyInput } | null>(null);
  const [saving, setSaving] = useState(false);
  const [reloading, setReloading] = useState(false);
  const [signingOut, setSigningOut] = useState(false);
  const [conflict, setConflict] = useState('');
  const [sessionExpired, setSessionExpired] = useState(false);
  const [renewing, setRenewing] = useState(false);
  const [renewed, setRenewed] = useState(false);
  const [renewalError, setRenewalError] = useState('');
  const busy = saving || reloading || signingOut || renewing;
  const writeBlocked = busy || !rulesAccepted;
  const [error, setError] = useState('');
  const [saved, setSaved] = useState(false);
  const dirty = JSON.stringify(draft) !== JSON.stringify(profileInput(profile));
  const dirtyRef = useRef(dirty || editor !== null);
  dirtyRef.current = dirty || editor !== null;

  useEffect(() => {
    const beforeUnload = (event: BeforeUnloadEvent) => { if (dirtyRef.current) event.preventDefault(); };
    window.addEventListener('beforeunload', beforeUnload);
    return () => window.removeEventListener('beforeunload', beforeUnload);
  }, []);
  useEffect(() => {
    onDirtyChange(dirty || editor !== null);
    return () => onDirtyChange(false);
  }, [dirty, editor, onDirtyChange]);
  useEffect(() => { setSessionExpired(false); }, [session.token]);

  function update(values: Partial<ProfileInput>) { setDraft((current) => ({ ...current, ...values })); setSaved(false); setRenewed(false); setError(''); }
  function requestFailed(cause: unknown) {
    if (cause instanceof ApiError && cause.code === 'RULES_ACCEPTANCE_REQUIRED') { onRulesRequired(); setError('Для сохранения профиля примите актуальные правила сообщества. Ваши изменения остались на экране.'); return; }
    if (needsSignIn(cause)) {
      setSessionExpired(true); setRenewed(false); setRenewalError(''); onSessionExpired();
    } else setError(errorMessage(cause));
  }
  async function renew() {
    if (busy) return;
    setRenewing(true); setRenewalError(''); setError(''); setSaved(false);
    try {
      await onRenewSession();
      setSessionExpired(false); setRenewed(true);
    } catch (cause) { setRenewalError(renewalErrorMessage(cause)); }
    finally { setRenewing(false); }
  }
  async function save(event: FormEvent) {
    event.preventDefault();
    if (writeBlocked || sessionExpired || conflict || editor) return;
    setSaving(true); setError(''); setSaved(false); setRenewed(false);
    try {
      const next = await saveProfile(session.token, draft, profile.revision);
      setDraft(profileInput(next)); onSaved(next); setSaved(true);
    } catch (cause) {
      if (needsProfileReload(cause)) setConflict(errorMessage(cause));
      else requestFailed(cause);
    } finally { setSaving(false); }
  }
  async function reload() {
    if (busy || sessionExpired) return;
    if ((dirty || editor) && !window.confirm('Загрузить профиль с сервера? Несохранённые изменения на этом экране будут заменены.')) return;
    setReloading(true); setError(''); setSaved(false); setRenewed(false);
    try {
      const next = await api<Profile>('/api/profile', { token: session.token });
      setDraft(profileInput(next)); setEditor(null); onSaved(next); setConflict('');
    } catch (cause) { requestFailed(cause); }
    finally { setReloading(false); }
  }
  async function signOut() {
    if (busy) return;
    if ((dirty || editor) && !window.confirm('Выйти из профиля? Несохранённые изменения будут потеряны.')) return;
    setSigningOut(true); setError('');
    try { await onLogout(); }
    catch (cause) { setError(`Не удалось выйти из профиля. ${errorMessage(cause)}`); }
    finally { setSigningOut(false); }
  }

  return <div className="profile-content">
    {!rulesAccepted && <p className="notice workflow-notice">Чтобы изменять профиль и предлагать помощь, <a className="text-button" href="#community-rules">примите правила сообщества</a>.</p>}
    <form id="profile-form" onSubmit={save}>
      <fieldset className="profile-fields" disabled={writeBlocked}>
        <div className="identity-row"><div className="avatar" aria-hidden="true">{draft.displayName.charAt(0).toUpperCase() || 'Я'}</div><div><strong>{profile.provenance === 'demo' ? 'Учебный профиль' : 'Ваш профиль'}</strong><p>{profile.provenance === 'demo' ? 'Вымышленный участник для проверки приложения' : 'Один профиль для вопросов и помощи другим'}</p></div></div>
        <div className="field-grid"><label className="field">Как к вам обращаться<input required value={draft.displayName} minLength={1} maxLength={100} autoComplete="nickname" onChange={(event) => update({ displayName: event.target.value })} /></label><label className="field">О себе <span className="optional">необязательно</span><input value={draft.bio} maxLength={500} placeholder="Чему учитесь, чем занимаетесь" onChange={(event) => update({ bio: event.target.value })} /></label></div>
        <div className="availability"><div><label className="switch-label"><input type="checkbox" checked={draft.availableToHelp} onChange={(event) => update({ availableToHelp: event.target.checked })} /><span className="switch" aria-hidden="true"/><strong>Готов(а) помогать</strong></label><p>Можно сделать паузу в любой момент. Ваши знания сохранятся.</p></div><label className="field capacity">Одновременно диалогов<select value={draft.maxActiveConversations} onChange={(event) => update({ maxActiveConversations: Number(event.target.value) })}>{[1, 2, 3, 4, 5].map((value) => <option key={value} value={value}>{value}</option>)}</select></label></div>
      </fieldset>
    </form>
    <div className="knowledge-heading"><div><h3>Могу объяснить <span className="count">{draft.competencies.length}</span></h3><p>Добавьте опыт учёбы, исследований или работы.</p></div>{!editor && <button type="button" className="button secondary small" disabled={writeBlocked || draft.competencies.length >= 30} onClick={() => { setEditor({ index: null, competency: newCompetency() }); setSaved(false); }}>Добавить <span aria-hidden="true">＋</span></button>}</div>
    {draft.competencies.length === 0 && !editor && <div className="empty-state"><div className="empty-icon" aria-hidden="true">✳</div><h4>У каждого есть что объяснить</h4><p>Поступление, олимпиада, стажировка или работа - начните с одного опыта, которым готовы поделиться.</p></div>}
    <div className="competencies">{draft.competencies.map((competency, index) => { const path = topicPath(competency.topicId, taxonomy.topics); return <article className="competency-card" key={competency.id ?? `${competency.topicId}-${index}`}><div className={`topic-icon topic-${path[0]?.id ?? 'digital'}`} aria-hidden="true">{({ pathways: '⌁', career: '↗' } as Record<string, string>)[path[0]?.id ?? ''] ?? '✳'}</div><div className="competency-info"><p className="competency-path">{path.slice(0, -1).map((topic) => topic.label).join(' / ')}</p><h4>{path.at(-1)?.label ?? 'Тема'}</h4><p>{competency.description || experienceLabels[competency.experienceKind]}</p><div className="competency-meta"><span>Указано самостоятельно</span>{competency.evidenceUrl && <span>Подтверждение не проверено · {competency.evidenceVisibility === 'private' ? 'только вам' : 'участникам'}</span>}</div></div><div className="card-actions"><button className="text-button" disabled={writeBlocked || editor !== null} onClick={() => setEditor({ index, competency: { ...competency, facets: { ...competency.facets } } })}>Изменить</button><button className="text-button muted" disabled={writeBlocked || editor !== null} aria-label={`Убрать знание: ${path.at(-1)?.label ?? 'тема'}`} onClick={() => update({ competencies: draft.competencies.filter((_, position) => position !== index) })}>Убрать</button></div></article>; })}</div>
    {!!profile.archivedCompetencies?.length && <details className="archived-competencies"><summary>Сохранённый опыт вне текущих направлений <span>{profile.archivedCompetencies.length}</span></summary><p>Эти знания остаются в вашем профиле. Сейчас они не участвуют в подборе: сообщество сосредоточено на образовании и карьере.</p><ul>{profile.archivedCompetencies.map((competency, index) => <li key={competency.id ?? `${competency.topicId}-${index}`}><strong>{topicPath(competency.topicId, taxonomy.topics).map(topic => topic.label).join(' → ') || competency.topicId}</strong>{competency.description && <p>{competency.description}</p>}<small>Сохранено в архиве · {experienceLabels[competency.experienceKind]}</small></li>)}</ul></details>}
    {editor && <fieldset className="profile-fields" disabled={writeBlocked}><CompetencyEditor key={editor.index ?? 'new'} initial={editor.competency} taxonomy={taxonomy} editing={editor.index !== null} onCancel={() => setEditor(null)} onAdd={(competency) => { update({ competencies: editor.index === null ? [...draft.competencies, competency] : draft.competencies.map((existing, index) => index === editor.index ? competency : existing) }); setEditor(null); }} /></fieldset>}
    {sessionExpired && <div className="notice notice-error" role="alert"><div><strong>Нужно войти снова</strong><p>Сессия закончилась. Несохранённые изменения остались на экране. Повторите вход, затем сохраните профиль.</p>{renewalError && <p>{renewalError}</p>}</div><button type="button" className="text-button" disabled={busy} onClick={() => void renew()}>{renewing ? 'Входим…' : 'Войти снова'}</button></div>}
    {renewed && <div className="notice" role="status">Вход восстановлен. Несохранённые изменения остались на экране; сохраните их, когда будете готовы.</div>}
    {error && <ErrorNotice message={error} />}
    {conflict && <div className="notice notice-error" role="alert"><span>{conflict}</span><div className="notice-actions"><button type="button" className="text-button" disabled={busy || sessionExpired} onClick={() => void reload()}>{reloading ? 'Загружаем…' : 'Загрузить актуальный профиль'}</button><small>При загрузке несохранённые изменения будут заменены.</small></div></div>}
    <div className="save-bar"><div aria-live="polite">{saved ? <span className="saved">✓ Профиль сохранён</span> : dirty ? <span>Есть несохранённые изменения</span> : <span>Знания можно дополнять в любое время</span>}</div><button className="button primary" type="submit" form="profile-form" disabled={writeBlocked || sessionExpired || Boolean(conflict) || editor !== null || (!dirty && !error)}>{saving ? 'Сохраняем…' : 'Сохранить профиль'} {!saving && <Arrow />}</button></div>
    <div className="session-actions"><button type="button" className="text-button" disabled={busy} onClick={() => void signOut()}>{signingOut ? 'Выходим…' : profile.provenance === 'demo' ? 'Выйти и сменить участника' : 'Выйти из профиля'}</button></div>
  </div>;
}

export function App() {
  const [bootstrap, setBootstrap] = useState<Bootstrap>();
  const [taxonomy, setTaxonomy] = useState<Taxonomy>();
  const [bridge, setBridge] = useState<BridgeSummary>({ state: 'loading', platform: 'Определяется…', hasLaunchData: false });
  const [session, setSession] = useState<Session>();
  const [loginSource, setLoginSource] = useState<LoginSource>();
  const [sessionConfirmed, setSessionConfirmed] = useState(false);
  const [profile, setProfile] = useState<Profile>();
  const [signedOut, setSignedOut] = useState(false);
  const [state, setState] = useState<'loading' | 'ready' | 'authenticating' | 'error'>('loading');
  const [error, setError] = useState('');
  const [attempt, setAttempt] = useState(0);
  const [persona, setPersona] = useState<'anna' | 'boris'>('anna');
  const [profileDirty, setProfileDirty] = useState(false);
  const [workflowDirty, setWorkflowDirty] = useState(false);
  const [moderationDirty, setModerationDirty] = useState(false);
  const [rulesRefresh, setRulesRefresh] = useState(0);
  const [rulesStatus, setRulesStatus] = useState<{ token: string; accepted: boolean } | null>(null);
  const rulesAccepted = Boolean(session && rulesStatus?.token === session.token && rulesStatus?.accepted);
  useEffect(() => {
    if (profileDirty || workflowDirty || moderationDirty) window.WebApp?.enableClosingConfirmation?.();
    else window.WebApp?.disableClosingConfirmation?.();
    return () => window.WebApp?.disableClosingConfirmation?.();
  }, [profileDirty, workflowDirty, moderationDirty, bridge.state]);

  useEffect(() => {
    let active = true;
    setState('loading'); setError(''); setSignedOut(false);
    async function start() {
      try {
        const [settings, catalog, webApp] = await Promise.all([api<Bootstrap>('/api/bootstrap'), api<Taxonomy>('/api/taxonomy'), loadBridge()]);
        if (!active) return;
        setBootstrap(settings); setTaxonomy(catalog); setBridge(bridgeSummary(webApp, webApp ? 'loaded' : 'unavailable'));
        webApp?.ready?.();
        if (webApp?.initData) {
          setState('authenticating');
          const nextSession = await api<Session>('/api/auth/max', { body: { initData: webApp.initData } });
          const nextProfile = await api<Profile>('/api/profile', { token: nextSession.token });
          if (!active) return;
          setSession(nextSession); setProfile(nextProfile); setLoginSource({ kind: 'max' }); setSessionConfirmed(true);
        }
        setState('ready');
      } catch (cause) { if (active) { setError(errorMessage(cause)); setState('error'); } }
    }
    void start();
    return () => { active = false; };
  }, [attempt]);

  async function demoLogin() {
    setState('authenticating'); setError('');
    try {
      const nextSession = await api<Session>('/api/auth/demo', { body: { persona } });
      const nextProfile = await api<Profile>('/api/profile', { token: nextSession.token });
      setSession(nextSession); setProfile(nextProfile); setLoginSource({ kind: 'demo', persona }); setSessionConfirmed(true); setState('ready');
    } catch (cause) { setError(errorMessage(cause)); setState('error'); }
  }

  async function renewSession() {
    if (!session || !loginSource) throw new Error('Missing session');
    const nextSession = await renewProfileSession(session, loginSource);
    // Keep ProfileForm mounted, including an open competency editor, and retain
    // the original profile revision so concurrent edits still cause a conflict.
    setSession(nextSession); setSessionConfirmed(true);
  }

  function reviewRules() { setRulesStatus(null); setRulesRefresh(value => value + 1); }

  async function signOut() {
    if (!session) return;
    if ((workflowDirty || moderationDirty) && !window.confirm('Выйти? Несохранённые вопросы, отклики, сообщения и обоснования решений будут потеряны.')) return;
    await logoutSession(session.token);
    setSession(undefined); setLoginSource(undefined); setSessionConfirmed(false); setProfile(undefined); setError(''); setSignedOut(true); setState('ready');
  }

  return <div className="app-shell">
    <a className="skip-link" href="#main">Перейти к содержимому</a>
    <header className="site-header"><a className="brand" href="#main" aria-label="Синапс - главная"><img src="/mark.svg" width="38" height="38" alt=""/><span>синапс<span className="brand-dot">.</span></span></a><nav aria-label="Основные разделы"><a className="nav-active" href="#main">Сообщество</a><a href="#knowledge">Мои знания</a>{session && <a href="#workspace">Вопросы</a>}</nav><span className="release-badge"><span aria-hidden="true"/>Учёба и карьера</span></header>
    <main id="main">
      <section className="hero" aria-labelledby="hero-title"><div className="hero-copy"><span className="eyebrow">ОПЫТ УЧЁБЫ И КАРЬЕРЫ - ОТ ЧЕЛОВЕКА К ЧЕЛОВЕКУ</span><h1 id="hero-title">Знать своё.<br/><span>Находить друг друга.</span></h1><p>Выбирайте следующий шаг в учёбе и карьере.<br className="desktop-break"/> Спрашивайте тех, кто уже прошёл похожий путь, и делитесь своим опытом.</p><a className="button primary hero-button" href={session ? '#workspace' : '#knowledge'}>{session ? 'Перейти к вопросам' : 'Начать'} <Arrow/></a></div><div className="knowledge-orbit" aria-hidden="true"><svg className="synapse-traces" viewBox="0 0 420 310" fill="none"><path d="M45 89C132 38 307 274 365 228M56 253C167 318 285-48 351 62"/><ellipse cx="210" cy="155" rx="179" ry="66" transform="rotate(-28 210 155)"/><circle cx="115" cy="91" r="4"/><circle cx="302" cy="220" r="4"/><circle cx="260" cy="69" r="3"/></svg><div className="orbit-ring ring-one"/><div className="orbit-ring ring-two"/><div className="orbit-center"><span>мой</span><strong>опыт</strong><svg viewBox="0 0 50 30" width="50" height="30" fill="none"><path d="M5 10h33l-6-6m13 16H12l6 6M38 10l-6 6M12 20l6-6" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"/></svg><strong>твой</strong><span>вопрос</span></div><span className="orbit-topic orbit-one">⌁ <b>Образование</b></span><span className="orbit-topic orbit-two">↗ <b>Карьера</b></span><span className="orbit-topic orbit-three">✳ <b>Исследования</b></span><span className="orbit-topic orbit-four">→ <b>Стажировки</b></span><span className="orbit-spark">✳</span></div></section>
      <section className="role-grid" aria-label="Две стороны одного сообщества"><article className="role-card role-learn"><span className="role-number">01 / УЧУСЬ</span><div className="role-title"><h2>Мне нужно разобраться</h2><Arrow/></div><p>Конкретный вопрос найдёт человека, которому знакома именно ваша тема.</p></article><article className="role-card role-help"><span className="role-number">02 / ДЕЛЮСЬ</span><div className="role-title"><h2>Здесь я могу помочь</h2><Arrow reverse/></div><p>Вы выбираете свои знания и откликаетесь, когда есть время и желание.</p></article></section>
      <div className="build-note"><span aria-hidden="true">◌</span><p>Один профиль для вопросов и помощи. Задать вопрос можно сразу. Чтобы получать вопросы других участников, добавьте знания и включите готовность помогать.</p></div>
      {profile && session && <section className="account-start" aria-labelledby="account-start-title"><div><span className={`account-badge ${loginSource?.kind === 'demo' ? 'account-demo' : ''}`}>{loginSource?.kind === 'demo' ? 'Учебный вход · вымышленный участник' : sessionConfirmed ? 'Вход через MAX подтверждён' : 'Нужно повторить вход'}</span><h2 id="account-start-title">{profile.displayName}, с чего начнём?</h2><p>Спрашивайте о незнакомом и делитесь тем, в чём разбираетесь. Добавлять знания перед первым вопросом необязательно.</p></div><div className="account-paths"><a className="account-path" href="#workspace"><strong>Мне нужна помощь →</strong><span>Вопрос → отклики → выбор собеседника → диалог</span></a><a className="account-path" href="#knowledge"><strong>Могу поделиться опытом →</strong><span>{profile.competencies.length ? 'Уточните знания и готовность помогать. Подходящие вопросы - в разделе «Могу помочь».' : 'Добавьте хотя бы одно знание, чтобы видеть подходящие вопросы.'}</span></a></div>{loginSource?.kind === 'demo' && <p className="account-demo-note">Изменения относятся к учебному участнику. Для личного профиля откройте приложение в MAX.</p>}</section>}
      {session && <CommunityRules key={session.user.id} refreshKey={rulesRefresh} session={session} onStatusChange={(token, accepted) => setRulesStatus({ token, accepted })} onSessionExpired={() => setSessionConfirmed(false)} onRenewSession={renewSession} />}
      {profile && session && taxonomy && <Workspace key={profile.id} rulesAccepted={rulesAccepted} onRulesRequired={reviewRules} profile={profile} session={session} taxonomy={taxonomy} onRenewSession={renewSession} onSessionExpired={() => setSessionConfirmed(false)} onDirtyChange={setWorkflowDirty} />}
      <section id="knowledge" className="profile-section" aria-labelledby="knowledge-title"><div className="section-heading"><div><span className="eyebrow">ЗНАНИЯ СТАНОВЯТСЯ ПОЛЕЗНЕЕ, КОГДА ИМИ ДЕЛЯТСЯ</span><h2 id="knowledge-title">Мои знания</h2></div><span className="section-step">01 - Профиль</span></div><p className="section-intro">Можно помочь собеседнику с поступлением и самому спросить о первой стажировке. Один профиль - для обеих сторон.</p>
        {bootstrap?.mode === 'demo' && !bridge.hasLaunchData && <div className="demo-banner"><strong>Демонстрационный режим</strong><span>Тестовые участники вымышлены. Данные сохраняются на локальном сервере.</span></div>}
        {(state === 'loading' || state === 'authenticating') && <div className="loading-state" role="status"><span className="spinner"/>{state === 'authenticating' ? 'Входим и загружаем профиль…' : 'Подключаемся к сообществу…'}</div>}
        {error && !profile && <ErrorNotice message={error} retry={() => setAttempt((current) => current + 1)} />}
        {profile && session && taxonomy ? <ProfileForm key={profile.id} rulesAccepted={rulesAccepted} onRulesRequired={reviewRules} profile={profile} session={session} taxonomy={taxonomy} onSaved={setProfile} onLogout={signOut} onRenewSession={renewSession} onSessionExpired={() => setSessionConfirmed(false)} onDirtyChange={setProfileDirty}/> : bootstrap && state !== 'loading' && state !== 'authenticating' && (!bridge.hasLaunchData && bootstrap.mode === 'demo' ? <div className="login-panel"><div><h3>Посмотрите, как устроен профиль</h3><p>Выберите учебного участника: можно задать вопрос об учёбе или карьере и добавить собственный опыт.</p></div><fieldset className="persona-picker"><legend className="sr-only">Учебный участник</legend><label className={persona === 'anna' ? 'persona selected' : 'persona'}><input type="radio" name="persona" checked={persona === 'anna'} onChange={() => setPersona('anna')}/><span className="persona-avatar lavender">А</span><span><strong>Анна</strong><small>Тестовый участник 01</small></span></label><label className={persona === 'boris' ? 'persona selected' : 'persona'}><input type="radio" name="persona" checked={persona === 'boris'} onChange={() => setPersona('boris')}/><span className="persona-avatar green">Б</span><span><strong>Борис</strong><small>Тестовый участник 02</small></span></label></fieldset><button className="button primary" onClick={() => void demoLogin()}>Открыть учебный профиль <Arrow/></button></div> : !bridge.hasLaunchData ? <div className="login-panel"><h3>Откройте приложение в MAX</h3><p>Вход доступен через мини-приложение бота. Открытие этой страницы в обычном браузере не подтверждает вашу личность.</p><button className="button secondary" onClick={() => setAttempt((current) => current + 1)}>Проверить подключение</button></div> : signedOut ? <div className="login-panel"><h3>Вы вышли из профиля</h3><p>Чтобы сменить участника, откройте мини-приложение из нужного аккаунта MAX.</p><button className="button secondary" onClick={() => setAttempt((current) => current + 1)}>Войти снова</button></div> : null)}
      </section>
      <section className="principle"><span className="principle-icon" aria-hidden="true">✳</span><div><h2>Один вопрос. Чей-то опыт.<br/>Новая точка понимания.</h2><p>Помочь одному человеку, научиться у другого. Взаимность без обязательного обмена один на один.</p></div><span className="principle-line" aria-hidden="true"/></section>
      {session && <ModerationWorkspace key={session.user.id} session={session} onDirtyChange={setModerationDirty} onSessionExpired={() => setSessionConfirmed(false)} onRenewSession={renewSession} />}
      {session && <NotificationSettings key={session.user.id} session={session} onSessionExpired={() => setSessionConfirmed(false)} onRenewSession={renewSession}/>}
      <Diagnostics bridge={bridge} bootstrap={bootstrap} authenticated={sessionConfirmed} state={state}/>
    </main><footer><span>Синапс · Synapse</span><span>Учиться и помогать - в одном сообществе</span><a href="#main">Наверх ↑</a></footer>
  </div>;
}
