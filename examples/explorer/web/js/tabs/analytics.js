/**
 * @file tabs/analytics.js
 * @brief The Analytics tab: detection modules and rules, and their simple parameters.
 *
 * ONVIF analytics settings are either *simple items* (a name and a text value,
 * such as Sensitivity = 60) or *structured items* (XML, such as a cell grid or a
 * polygon). Simple items are editable here; structured items are shown as XML
 * and sent back unchanged.
 */

import { api } from '../api.js';
import { card, goodNote, h, indentXml, note, row, wideCard } from '../dom.js';
import { errorBox, problem } from '../errors.js';
import { base } from '../state.js';

export async function renderAnalytics(fill, current) {
  const reply = await api('GET', base() + '/analytics');
  if (!current()) return;
  if (!reply.ok) return fill(problem(reply.error));
  if (!reply.data.length) return fill(note('No profile has a video analytics configuration.'));
  fill(...reply.data.map(configurationGrid));
}

/** One analytics configuration: which profiles use it, then a card per module and rule. */
function configurationGrid(entry) {
  const cards = (section, kind, title) =>
    section.ok
      ? section.value.map((config) => analyticsCard(entry.configuration, kind, config))
      : [card(title, problem(section.error))];
  return h(
    'div',
    { class: 'grid' },
    wideCard(
      `Analytics configuration ${entry.configuration}`,
      h('div', { class: 'muted', text: 'Used by ' + entry.profiles.join(', ') }),
    ),
    cards(entry.modules, 'module', 'Modules'),
    cards(entry.rules, 'rule', 'Rules'),
  );
}

/** A module or rule with an input per simple parameter. */
function analyticsCard(configuration, kind, config) {
  const inputs = config.parameters.map((parameter) => [
    parameter,
    h('input', { value: parameter.value, 'aria-label': parameter.name }),
  ]);
  const result = h('div', {});
  const apply = h('button', { class: 'primary', type: 'button', text: 'Apply' });
  apply.onclick = async () => {
    // Send only the parameters that changed.
    const values = {};
    for (const [parameter, input] of inputs) {
      if (input.value !== parameter.value) values[parameter.name] = input.value;
    }
    if (!Object.keys(values).length) return result.replaceChildren(note('Nothing changed.'));
    const body = { configuration, kind, name: config.name, values };
    const reply = await api('POST', base() + '/analytics', body, { quiet: true });
    if (!reply.ok) return result.replaceChildren(errorBox(reply.error));
    for (const [parameter, input] of inputs) parameter.value = input.value;
    result.replaceChildren(goodNote('Applied.'));
  };

  const parameterRow = ([parameter, input]) =>
    h('div', { class: 'slider' }, h('span', { text: parameter.name }), input, h('span', {}));
  const structured = (item) =>
    h(
      'details',
      {},
      h('summary', { text: `${item.name} (structured, kept unchanged)` }),
      h('pre', { text: indentXml(item.xml) }),
    );

  return card(
    `${kind === 'rule' ? 'Rule' : 'Module'} ${config.name}`,
    h('div', { class: 'muted small mono', text: config.type }),
    inputs.length
      ? h('div', {}, inputs.map(parameterRow))
      : h('p', { class: 'muted', text: 'No simple parameters.' }),
    config.structured.map(structured),
    inputs.length ? row(apply) : null,
    result,
  );
}
