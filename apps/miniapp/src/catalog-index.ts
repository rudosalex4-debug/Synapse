import type { Taxonomy, Topic } from './types';
const normalize = (value: string) => value.toLocaleLowerCase('ru').replaceAll('ё', 'е').trim();
export function buildCatalogIndex(taxonomy: Taxonomy) {
  // Historical paths remain readable; only active nodes enter choices and search.
  const allById = new Map(taxonomy.topics.map(topic => [topic.id, topic]));
  const byId = new Map(taxonomy.topics.filter(topic => topic.active).map(topic => [topic.id, topic]));
  const children = new Map<string | null, Topic[]>();
  for (const topic of byId.values()) { const list = children.get(topic.parent_id) ?? []; list.push(topic); children.set(topic.parent_id, list); }
  function path(id: string): Topic[] {
    const result: Topic[] = [], seen = new Set<string>();
    let topic = allById.get(id);
    while (topic && !seen.has(topic.id)) {
      seen.add(topic.id); result.unshift(topic);
      topic = topic.parent_id ? allById.get(topic.parent_id) : undefined;
    }
    return result;
  }
  const entries = [...byId.values()].filter(topic => topic.level >= 2).map(topic => {
    const chain = path(topic.id);
    return { topic, breadcrumb: chain.map(item => item.label).join(' → '),
      searchable: normalize(chain.flatMap(item => [item.label, item.description ?? '', ...(item.aliases ?? [])]).join(' ')) };
  });
  function search(query: string, limit = 8) {
    const normalized = normalize(query);
    if (normalized.length < 2) return [];
    const terms = normalized.split(/\s+/);
    return entries.filter(entry => terms.every(term => entry.searchable.includes(term)))
      .sort((a, b) => Number(normalize(b.topic.label) === normalized) - Number(normalize(a.topic.label) === normalized)
        || a.topic.level - b.topic.level || a.topic.label.localeCompare(b.topic.label, 'ru')).slice(0, limit);
  }
  return { byId, children, path, search };
}
