import { useMemo, useState } from 'react';
import { buildCatalogIndex } from './catalog-index';
import { topicPath } from './profile-model';
import type { Taxonomy, Topic } from './types';

const trackCopy: Record<string, { kicker: string; intro: string; example: string; tags: string[] }> = {
  pathways: {
    kicker: 'УЧИТЬСЯ И ИССЛЕДОВАТЬ',
    intro: 'От выбора школы и университета до курсов, олимпиад и первых исследований.',
    example: 'Университет → поступление → вуз и специальность',
    tags: ['Поступление', 'Студенческая жизнь', 'Наука'],
  },
  career: {
    kicker: 'ПРОБОВАТЬ И РАСТИ',
    intro: 'От первой стажировки до смены профессии и развития в своей специальности.',
    example: 'Стажировка → отбор → компания и направление',
    tags: ['Стажировки', 'Работа', 'Профессиональный рост'],
  },
};

function TrackArtwork({ career }: { career: boolean }) {
  return <svg className="track-artwork" viewBox="0 0 180 115" fill="none" aria-hidden="true">
    <circle cx="124" cy="55" r="42" fill="currentColor" opacity=".06" />
    <ellipse cx="94" cy="60" rx="78" ry="19" transform="rotate(-22 94 60)" stroke="currentColor" strokeOpacity=".18" />
    <path d="M13 81C47 112 128 5 165 35" stroke="currentColor" strokeOpacity=".22" strokeDasharray="2 5" />
    <circle cx="155" cy="30" r="3" fill="currentColor" opacity=".32" />
    <path d="M12 98h154" stroke="currentColor" strokeOpacity=".2" />
    {career ? <>
      <path d="M20 95V78h36V57h36V37h38V17h33" stroke="currentColor" strokeWidth="2" strokeLinejoin="round" />
      <path d="m148 7 15 10-15 10" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
      <rect x="27" y="40" width="43" height="29" rx="5" fill="var(--track-surface)" stroke="currentColor" strokeWidth="2" />
      <path d="M40 40v-6h17v6M28 51h41m-21-4v8" stroke="currentColor" strokeWidth="2" strokeLinejoin="round" />
      <circle cx="100" cy="71" r="6" fill="currentColor" opacity=".3" />
      <circle cx="137" cy="50" r="5" stroke="currentColor" strokeWidth="2" />
    </> : <>
      <path d="M30 88V40c19-6 34-4 49 6v47c-15-10-30-12-49-5Zm98 0V40c-19-6-34-4-49 6v47c15-10 30-12 49-5Z" fill="var(--track-surface)" stroke="currentColor" strokeWidth="2" strokeLinejoin="round" />
      <path d="M42 53c9-1 17 1 25 5m-25 9c9-1 17 1 25 5m25-14c7-4 16-6 24-5m-24 19c7-4 16-6 24-5" stroke="currentColor" strokeOpacity=".4" strokeWidth="2" strokeLinecap="round" />
      <path d="m100 17 24-9 24 9-24 10-24-10Zm10 5v10c9 6 19 6 28 0V22m10-5v16" fill="var(--track-surface)" stroke="currentColor" strokeWidth="2" strokeLinejoin="round" />
    </>}
    <circle cx="19" cy="27" r="3" fill="currentColor" opacity=".3" />
  </svg>;
}

/** One selection path shared by asking and offering experience. Only active catalog topics are selectable. */
export function TopicJourney({ taxonomy, value, onChange, purpose }: { taxonomy: Taxonomy; value: string; onChange: (id: string) => void; purpose: 'question' | 'competency' }) {
  const index = useMemo(() => buildCatalogIndex(taxonomy), [taxonomy]);
  const [query, setQuery] = useState('');
  const path = index.byId.has(value) ? index.path(value) : [];
  const track = path.find(topic => topic.level === 1);
  const branch = path.find(topic => topic.level === 2);
  const leaf = path.at(-1);
  const inactive = !!value && !index.byId.has(value);
  const historicalLabel = inactive ? topicPath(value, taxonomy.topics).map(topic => topic.label).join(' → ') : '';
  const choose = (id: string) => { onChange(id); setQuery(''); };
  const matches = query.trim().length >= 2 ? index.search(query, 80).filter(result => !track || result.topic.id === track.id || index.path(result.topic.id).some(topic => topic.id === track.id)).slice(0, 10) : [];
  const nextTopics = leaf ? index.children.get(leaf.id) ?? [] : [];
  const branches = track ? index.children.get(track.id) ?? [] : [];
  const copy = track ? trackCopy[track.id] : undefined;
  function option(topic: Topic) {
    return <button key={topic.id} type="button" className="journey-option" onClick={() => choose(topic.id)}><span><strong>{topic.label}</strong>{topic.description && <small>{topic.description}</small>}</span><span aria-hidden="true">↗</span></button>;
  }
  return <section className="topic-journey" aria-label={purpose === 'question' ? 'Направление вопроса' : 'Направление опыта'}>
    <div className="journey-heading"><span className="journey-step">01</span><div><h4>{purpose === 'question' ? 'Куда направить вопрос' : 'Каким опытом вы поделитесь'}</h4><p>Выберите направление, затем свою ситуацию. Уточнения помогут найти близкий опыт.</p></div></div>
    {inactive && <div className="archived-topic-notice" role="status"><strong>Эта тема сохранена в архиве</strong><p>{historicalLabel || value}. Сейчас вопросы и помощь доступны в двух направлениях. Выберите актуальную тему ниже; ваш текст останется на месте.</p></div>}
    {!track ? <div className="track-choices">{(index.children.get(null) ?? []).map(topic => {
      const item = trackCopy[topic.id];
      return <button type="button" key={topic.id} className={`track-choice track-${topic.id}`} onClick={() => choose(topic.id)}><span className="track-kicker">{item?.kicker ?? 'ВЫБРАТЬ НАПРАВЛЕНИЕ'}</span><TrackArtwork career={topic.id === 'career'} /><strong className="track-title">{topic.label}</strong><span className="track-description">{item?.intro ?? topic.description}</span><span className="track-tags">{item?.tags.map(tag => <span key={tag}>{tag}</span>)}</span><span className="track-example">{item?.example}</span><span className="track-action">Выбрать направление <span aria-hidden="true">→</span></span></button>;
    })}</div> : <div className={`selected-track track-${track.id}`}><span className="selected-track-symbol" aria-hidden="true">{track.id === 'career' ? '↗' : '⌁'}</span><div><small>{copy?.kicker ?? 'ВАШЕ НАПРАВЛЕНИЕ'}</small><strong>{track.label}</strong></div><button className="text-button" type="button" onClick={() => choose('')}>Сменить</button></div>}
    {path.length > 0 && <nav className="journey-breadcrumbs" aria-label="Выбранный путь"><ol>{path.map((topic, position) => <li key={topic.id}>{position > 0 && <span aria-hidden="true">/</span>}<button type="button" aria-current={position === path.length - 1 ? 'step' : undefined} onClick={() => choose(topic.id)}>{topic.label}</button></li>)}</ol></nav>}
    <label className="field journey-search">{track ? `Найти тему: ${track.label.toLocaleLowerCase('ru')}` : 'Или найдите свою ситуацию'}<input type="search" value={query} onChange={event => setQuery(event.target.value)} placeholder={track?.id === 'career' ? 'Например: стажировка, собеседование, смена профессии' : 'Например: поступление, олимпиада, исследование'} /><small>Поиск по темам и их названиям. Конкретный вуз, компания и специальность выбираются следующим шагом.</small></label>
    {query.trim().length >= 2 ? <div className="topic-search-results journey-search-results" aria-live="polite">{matches.length ? <><ul>{matches.map(({ topic, breadcrumb }) => <li key={topic.id}><button type="button" onClick={() => choose(topic.id)}>{breadcrumb}</button></li>)}</ul><small>Показаны первые {matches.length} совпадений. Уточните запрос, если нужной темы нет.</small></> : <p>Такой темы пока нет. Выберите ближайшую ситуацию и опишите подробности своими словами.</p>}</div> : track && !branch ? <div className="journey-branches"><h5>С чем связан ваш опыт или вопрос?</h5><div className="journey-options">{branches.map(option)}</div></div> : branch && <div className="journey-refinement"><div className="journey-selection"><div><span className="journey-selection-label">Выбранная тема</span><strong>{leaf?.label}</strong>{leaf?.description && <p>{leaf.description}</p>}</div>{leaf?.parent_id && <button type="button" className="text-button" onClick={() => choose(leaf.parent_id!)}>← На шаг назад</button>}</div>{nextTopics.length > 0 && <label className="field">Уточните ситуацию <span className="optional">если нужно</span><select value="" onChange={event => { if (event.target.value) choose(event.target.value); }}><option value="">Оставить тему «{leaf?.label}»</option>{nextTopics.map(topic => <option key={topic.id} value={topic.id}>{topic.label}</option>)}</select><small>Можно оставить общую тему, если вопрос затрагивает несколько её частей.</small></label>}</div>}
    {!branch && <p className="journey-required">Для сохранения выберите хотя бы одну тему внутри направления.</p>}
  </section>;
}
