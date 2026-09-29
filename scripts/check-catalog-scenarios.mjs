import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const catalog = JSON.parse(readFileSync(new URL('../data/taxonomy.example.json', import.meta.url)));
export const scenarios = JSON.parse(readFileSync(new URL('../data/day18-acceptance-scenarios.json', import.meta.url)));
assert.equal(scenarios.catalogVersion, catalog.version, 'Acceptance scenarios must use the current catalog version');
const topics = new Map(catalog.topics.map(t => [t.id,t]));
const facets = new Map(catalog.facets.map(f => [f.id,f]));
function validate(subject) {
  const topic = topics.get(subject.topicId);
  assert.ok(topic?.active && topic.level >= 2, 'Scenario must select an active L2+ topic');
  for(const [key, values] of Object.entries(subject.facets)) {
    const facet=facets.get(key);
    assert.ok(facet, 'Unknown facet');
    assert.ok(facet.applicable_topic_prefixes.some(p => p === '*' || subject.topicId === p || subject.topicId.startsWith(p+'.')), 'Facet does not apply');
    assert.ok(values.length > 0 && new Set(values).size === values.length);
    for (const value of values) assert.ok(facet.values.some(v => v.id === value), 'Unknown facet value');
  }
}
const identities = new Map(scenarios.scenarios.map(s => [s.id,s]));
assert.equal(identities.size, scenarios.scenarios.length);
const roots = new Set(), audiences = new Set(), helpers = new Set();
for (const scenario of scenarios.scenarios) {
  audiences.add(scenario.audience);
  assert.ok(scenario.profile.competencies.length);
  scenario.profile.competencies.forEach(skill => {validate(skill); roots.add(skill.topicId.split('.')[0]);});
  validate(scenario.question);
  assert.notEqual(scenario.id, scenario.question.expectedHelper);
  const helper = identities.get(scenario.question.expectedHelper);
  assert.ok(helper?.profile.competencies.some(s => s.topicId === scenario.question.topicId &&
    Object.entries(scenario.question.facets).every(([key,values]) => values.every(value => s.facets[key]?.includes(value)))),
    'Expected helper must cover the topic and question facets in one competency');
  helpers.add(helper.id);
  // This validates fixture consistency; runtime matching has separate tests.
}
const activeRoots = catalog.topics.filter(t => t.active && t.level === 1).map(t => t.id).sort();
assert.deepEqual([...roots].sort(),activeRoots,'Scenarios must cover every active catalog domain');
assert.deepEqual([...helpers].sort(),[...identities.keys()].sort(),'Every participant must also help another participant');
assert.deepEqual([...audiences].sort(),['school','student','worker']);
if (process.argv[1]?.replaceAll('\\','/').endsWith('/check-catalog-scenarios.mjs'))
  console.log(`PASS ${scenarios.scenarios.length} synthetic scenarios / ${roots.size} active domains / ${audiences.size} audiences; every participant asks and helps. Fixture consistency only; runtime matching is checked separately.`);
