import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const catalog = JSON.parse(readFileSync(new URL('../data/taxonomy.example.json', import.meta.url)));
export const scenarios = JSON.parse(readFileSync(new URL('../data/day18-acceptance-scenarios.json', import.meta.url)));
assert.equal(scenarios.catalogVersion, catalog.version);
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
const roots = new Set(), audiences = new Set();
for (const scenario of scenarios.scenarios) {
  audiences.add(scenario.audience);
  assert.ok(scenario.profile.competencies.length);
  scenario.profile.competencies.forEach(skill => {validate(skill); roots.add(skill.topicId.split('.')[0]);});
  validate(scenario.question);
  assert.notEqual(scenario.id, scenario.question.expectedHelper);
  const helper = identities.get(scenario.question.expectedHelper);
  assert.ok(helper?.profile.competencies.some(s => s.topicId === scenario.question.topicId), 'Expected helper lacks the specific topic');
  // This validates fixture consistency, not the future matching algorithm.
}
assert.equal(roots.size,5);
assert.deepEqual([...audiences].sort(),['school','student','worker']);
if (process.argv[1]?.replaceAll('\\','/').endsWith('/check-catalog-scenarios.mjs'))
  console.log('PASS 5 synthetic scenarios / 5 domains / 3 audiences; every participant asks and helps. Matching is not implemented.');
