import { describe, expect, it } from 'vitest';
import { buildCatalogIndex } from './catalog-index';
import type { Taxonomy } from './types';
const catalog: Taxonomy = {version:'test',facets:[],topics:[
  {id:'digital',parent_id:null,level:1,label:'Цифровые инструменты',active:true},
  {id:'digital.tables',parent_id:'digital',level:2,label:'Электронные таблицы',aliases:['Excel'],active:true},
  {id:'digital.tables.pivot',parent_id:'digital.tables',level:3,label:'Сводные таблицы',active:true},
  {id:'digital.tables.pivot.group',parent_id:'digital.tables.pivot',level:4,label:'Группировка дат',active:true},
  {id:'digital.hidden',parent_id:'digital',level:2,label:'Скрыто',active:false},
]};
describe('индекс каталога и поиск тем',()=>{
  const index=buildCatalogIndex(catalog);
  it('строит путь L4 без жёсткого ограничения на три уровня',()=>expect(index.path('digital.tables.pivot.group').map(t=>t.level)).toEqual([1,2,3,4]));
  it('группирует только непосредственных детей',()=>expect(index.children.get('digital')?.map(t=>t.id)).toEqual(['digital.tables']));
  it('находит по синониму и пути независимо от регистра',()=>expect(index.search('EXCEL даты'.replace('даты','дат'))[0]?.topic.id).toBe('digital.tables.pivot.group'));
  it('скрывает выключенные темы',()=>expect(index.search('скрыто')).toEqual([]));
  it('не создаёт шумную выдачу на один символ',()=>expect(index.search('э')).toEqual([]));
  it('ограничивает длину списка',()=>expect(index.search('таблицы',1)).toHaveLength(1));
});
