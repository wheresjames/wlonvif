/**
 * @file tabs/overview.js
 * @brief The Overview tab: identity, clock, service endpoints, and scopes.
 */

import { api } from '../api.js';
import { card, facts, h, pill, shortNamespace, table, wideCard } from '../dom.js';
import { problem, section } from '../errors.js';
import { base } from '../state.js';

export async function renderOverview(fill) {
  const reply = await api('GET', base() + '/overview');
  if (!reply.ok) return fill(problem(reply.error));
  const view = reply.data;
  fill(
    h(
      'div',
      { class: 'grid' },
      card('Identity', section(view.identity, identityFacts)),
      card('Clock', section(view.clock, clockFacts)),
      card('Service endpoints', section(view.capabilities, endpointFacts)),
      wideCard(
        'Advertised services',
        section(view.services, (services) =>
          table(['Service', 'wlonvif', 'Address'], services.map(serviceRow)),
        ),
      ),
      wideCard(
        'Scopes',
        section(view.scopes, (scopes) => table(null, scopes.map(scopeRow))),
      ),
    ),
  );
}

const identityFacts = (value) =>
  facts([
    ['Manufacturer', value.manufacturer],
    ['Model', value.model],
    ['Firmware', value.firmware],
    ['Serial number', value.serial],
    ['Hardware', value.hardware],
  ]);

/** The clock, with its offset from this computer; a large offset breaks sign-in on some cameras. */
function clockFacts(value) {
  const offset = Math.abs(value.skew_seconds);
  return facts([
    ['Camera UTC', value.utc],
    [
      'Offset',
      h('span', {
        class: offset > 5 ? 'warn' : 'good',
        text:
          offset <= 2 ? 'in step with this computer' : `${value.skew_seconds} s from this computer`,
      }),
    ],
    ['Mode', value.mode === 'ntp' ? 'NTP' : 'Manual'],
    ['Time zone', value.time_zone || '(not reported)'],
    ['Daylight saving', value.daylight_savings ? 'on' : 'off'],
  ]);
}

/** Where the camera says each service is; the library uses these addresses. */
const endpointFacts = (value) =>
  facts(
    Object.entries(value).map(([name, url]) => [
      name,
      url
        ? h('span', { class: 'mono', text: url })
        : h('span', { class: 'muted', text: 'not advertised' }),
    ]),
  );

/** An advertised service, and whether wlonvif can use it. */
function serviceRow(service) {
  const supported = service.support === 'supported';
  return h(
    'tr',
    {},
    h('td', { class: 'mono', text: shortNamespace(service.namespace) }),
    h(
      'td',
      {},
      pill(supported ? 'supported' : 'not supported', supported ? 'good' : 'warn'),
      supported ? null : h('div', { class: 'muted small', text: service.support }),
    ),
    h('td', { class: 'mono', text: service.xaddr }),
  );
}

const scopeRow = (scope) =>
  h(
    'tr',
    {},
    h('td', {}, pill(scope.fixed ? 'fixed' : 'configurable', scope.fixed ? '' : 'good')),
    h('td', { class: 'mono', text: decodeURIComponent(scope.uri) }),
  );
