import { useState } from 'react';
import { applicableFacets } from './profile-model';
import type { Facet, Taxonomy } from './types';

const normalize = (text: string) => text.toLocaleLowerCase('ru').replaceAll('ё', 'е').trim();

function ContextPicker({ facet, selected, open, onOpen, onChange, required, onRequiredChange }: { facet: Facet; selected: string[]; open: boolean; onOpen: () => void; onChange: (values: string[]) => void; required?: boolean; onRequiredChange?: (required: boolean) => void }) {
  const [search, setSearch] = useState('');
  const matches = facet.values.filter(value => normalize(value.label).includes(normalize(search)));
  const shown = matches.slice(0, 12);
  const selectedValues = facet.values.filter(value => selected.includes(value.id));
  return <div className={open ? 'context-picker is-open' : 'context-picker'}>
    <button type="button" className="context-picker-toggle" aria-expanded={open} onClick={onOpen}><span><strong>{facet.label}</strong><small>{selectedValues.length ? selectedValues.map(value => value.label).join(' · ') : facet.placeholder ?? 'Не указано - выберите, если это важно'}</small></span><span className="context-picker-mark" aria-hidden="true">{open ? '−' : '+'}</span></button>
    {open && <div className="context-picker-body">
      {facet.help && <p className="field-hint">{facet.help}</p>}
      {!!selectedValues.length && <ul className="context-selected" aria-label={`Выбрано: ${facet.label}`}>{selectedValues.map(value => <li key={value.id}><button type="button" onClick={() => onChange(selected.filter(id => id !== value.id))} aria-label={`Убрать: ${value.label}`}>{value.label}<span aria-hidden="true">×</span></button></li>)}</ul>}
      {facet.values.length > 7 && <label className="field context-search"><span className="sr-only">Поиск: {facet.label}</span><input type="search" value={search} onChange={event => setSearch(event.target.value)} placeholder={`Найти: ${facet.label.toLocaleLowerCase('ru')}`} /></label>}
      <fieldset className="context-value-list"><legend className="sr-only">{facet.label}</legend>{shown.map(value => <label key={value.id}><input type="checkbox" checked={selected.includes(value.id)} onChange={event => onChange(event.target.checked ? [...selected, value.id] : selected.filter(id => id !== value.id))} /><span>{value.label}</span></label>)}</fieldset>
      {matches.length > shown.length && <p className="field-hint">Показано {shown.length} из {matches.length}. Введите название, чтобы сузить список.</p>}
      {!matches.length && <p className="field-hint" role="status">В списке нет такого значения. Оставьте уточнение пустым и укажите название в тексте. По нему пока не будет точного подбора.</p>}
      {onRequiredChange && selectedValues.length > 0 && <label className="required-facet"><input type="checkbox" checked={required ?? false} onChange={event => onRequiredChange(event.target.checked)} />Нужен помощник именно с этим опытом</label>}
    </div>}
  </div>;
}

/** Facet values are selected from the shared catalog; free text never becomes an exact-match identifier. */
export function ContextFields({ topicId, taxonomy, value, onChange, requiredFacets, onRequiredChange }: { topicId: string; taxonomy: Taxonomy; value: Record<string, string[]>; onChange: (facetId: string, values: string[]) => void; requiredFacets?: string[]; onRequiredChange?: (facetId: string, required: boolean) => void }) {
  const facets = applicableFacets(topicId, taxonomy.facets);
  const [open, setOpen] = useState<string | null>(null);
  if (!facets.length) return null;
  return <section className="context-fields" aria-label="Контекст выбранной темы"><div className="journey-heading"><span className="journey-step">02</span><div><h4>Добавьте конкретику</h4><p>{onRequiredChange ? 'Выберите важные обстоятельства: вуз, компанию, специальность или этап. Все уточнения необязательны.' : 'Укажите, где и в каких обстоятельствах вы получили опыт. Все уточнения необязательны.'}</p></div></div><div className="context-pickers">{facets.map(facet => <ContextPicker key={`${topicId}:${facet.id}`} facet={facet} selected={value[facet.id] ?? []} open={open === facet.id} onOpen={() => setOpen(current => current === facet.id ? null : facet.id)} onChange={values => onChange(facet.id, values)} required={requiredFacets?.includes(facet.id)} onRequiredChange={onRequiredChange ? required => onRequiredChange(facet.id, required) : undefined} />)}</div><p className="context-footnote">Если в списке нет вашего вуза, компании или курса, укажите название в описании. Оно будет видно собеседнику, но не станет точным условием подбора.{onRequiredChange && ' Обязательные условия сужают круг помощников.'}</p></section>;
}
