/**
 * @file tabs/device.js
 * @brief The Device tab: clock, NTP, network, accounts, scopes, and reboot.
 *
 * Each card shows what the camera reports and offers the matching changes.
 * After a change succeeds the tab reloads, so it shows what the camera actually
 * stored; cameras often accept a value and keep a different one.
 */

import { api } from '../api.js';
import {
  card,
  facts,
  goodNote,
  h,
  pill,
  row,
  setUnavailable,
  table,
  toast,
  wideCard,
} from '../dom.js';
import { errorBox, problem, section, sectionUnsupported } from '../errors.js';
import { showTab } from '../tab-bar.js';
import { base } from '../state.js';

export async function renderDevice(fill, current) {
  const reply = await api('GET', base() + '/device');
  if (!current()) return;
  if (!reply.ok) return fill(problem(reply.error));
  const view = reply.data;
  fill(
    h(
      'div',
      { class: 'grid' },
      clockCard(view),
      ntpCard(view),
      networkCard(view),
      card(
        'Accounts',
        section(view.users, (list) => facts(list.map((user) => [user.level, user.username]))),
      ),
      scopesCard(view),
      maintenanceCard(),
    ),
  );
}

/**
 * A device action whose outcome appears in @p holder: the error there, or a
 * toast and a reload of the tab.
 */
async function act(body, message, holder) {
  const result = await api('POST', base() + '/device', body, { quiet: true });
  if (!result.ok) {
    holder.replaceChildren(problem(result.error));
    return;
  }
  toast(message, 'good');
  showTab();
}

const button = (text, onclick, tone) =>
  h('button', { class: tone || null, type: 'button', text, onclick });

/** @p element, dimmed when the camera answered @p value with "not supported". */
function needs(value, what, element) {
  // A setting the camera cannot read is one it cannot change either.
  if (sectionUnsupported(value)) setUnavailable(`The camera does not support ${what}.`, element);
  return element;
}

// ---- Clock ------------------------------------------------------------------

function clockCard(view) {
  const clock = view.clock.ok ? view.clock.value : null;
  const result = h('div', {});
  const zone = h('input', {
    value: clock?.time_zone || '',
    placeholder: 'e.g. CET-1CEST,M3.5.0,M10.5.0/3',
    'aria-label': 'POSIX time zone',
  });
  const dst = h('input', {
    type: 'checkbox',
    checked: clock?.daylight_savings,
    'aria-label': 'Daylight saving',
  });
  const what = 'reading or setting its clock';
  const setZone = () =>
    act(
      { action: 'time_zone', time_zone: zone.value || undefined, dst: dst.checked },
      'Time zone set',
      result,
    );
  return card(
    'Clock',
    section(view.clock, (value) =>
      facts([
        ['Camera UTC', value.utc],
        ['Offset', `${value.skew_seconds} s from this computer`],
        ['Mode', value.mode === 'ntp' ? 'NTP' : 'Manual'],
      ]),
    ),
    needs(
      view.clock,
      what,
      row(
        button("Set to this computer's time", () =>
          act({ action: 'sync_time' }, 'Clock set', result),
        ),
        button('Use NTP', () => act({ action: 'ntp_mode' }, 'Clock uses NTP', result)),
      ),
    ),
    needs(
      view.clock,
      what,
      row(zone, h('label', { class: 'row' }, dst, 'Daylight saving'), button('Apply', setZone)),
    ),
    result,
  );
}

// ---- NTP --------------------------------------------------------------------

function ntpCard(view) {
  const ntp = view.ntp.ok ? view.ntp.value : null;
  const result = h('div', {});
  const fromDhcp = h('input', {
    type: 'checkbox',
    checked: ntp?.from_dhcp,
    'aria-label': 'NTP from DHCP',
  });
  const servers = h('input', {
    value: (ntp?.servers || []).join(' '),
    placeholder: 'pool.ntp.org 192.0.2.1',
    'aria-label': 'NTP servers',
  });
  const apply = () =>
    act(
      {
        action: 'ntp_servers',
        dhcp: fromDhcp.checked,
        // Spaces or commas between servers.
        servers: fromDhcp.checked ? [] : servers.value.split(/[\s,]+/).filter(Boolean),
      },
      'NTP updated',
      result,
    );
  return card(
    'NTP',
    section(view.ntp, (value) =>
      facts([
        ['Source', value.from_dhcp ? 'DHCP' : 'manual'],
        ['Servers', value.servers.join(', ') || '—'],
        ['From DHCP', value.dhcp_servers.join(', ') || '—'],
      ]),
    ),
    needs(
      view.ntp,
      'NTP settings',
      row(h('label', { class: 'row' }, fromDhcp, 'From DHCP'), servers, button('Apply', apply)),
    ),
    result,
  );
}

// ---- Network ----------------------------------------------------------------

function networkCard(view) {
  const result = h('div', {});
  const hostname = h('input', {
    value: view.hostname.ok ? view.hostname.value.name : '',
    'aria-label': 'Hostname',
  });
  const setHostname = () =>
    act(
      { action: 'hostname', name: hostname.value },
      'Hostname set; some cameras restart their network',
      result,
    );
  return card(
    'Network',
    section(view.interfaces, (list) =>
      h(
        'div',
        {},
        list.map((item) =>
          facts([
            ['Interface', `${item.name} ${item.enabled ? '' : '(disabled)'}`],
            ['MAC', item.mac],
            ['IPv4', item.ipv4.join(', ') + (item.dhcp ? ' (DHCP)' : '')],
            ['MTU', item.mtu],
          ]),
        ),
      ),
    ),
    section(view.gateway, (list) => facts([['Gateway', list.join(', ') || '—']])),
    section(view.dns, (value) => {
      const servers = value.from_dhcp ? value.dhcp_servers : value.servers;
      return facts([
        ['DNS', servers.join(', ') + (value.from_dhcp ? ' (DHCP)' : '')],
        ['Search', value.search.join(', ')],
      ]);
    }),
    section(view.protocols, (list) =>
      facts(
        list.map((protocol) => [
          protocol.name,
          (protocol.enabled ? 'port ' : 'disabled, port ') + protocol.ports.join(', '),
        ]),
      ),
    ),
    section(view.discoverable, (value) => facts([['Discoverable', value ? 'yes' : 'no']])),
    needs(view.hostname, 'hostname settings', row(hostname, button('Set hostname', setHostname))),
    section(view.discoverable, (discoverable) =>
      row(
        button(discoverable ? 'Hide from WS-Discovery' : 'Make discoverable', () =>
          act({ action: 'discoverable', enabled: !discoverable }, 'Discovery mode changed', result),
        ),
      ),
    ),
    result,
  );
}

// ---- Scopes -----------------------------------------------------------------

function scopesCard(view) {
  const result = h('div', {});
  const location = h('input', {
    placeholder: 'Location, e.g. Front door',
    'aria-label': 'Location',
  });
  const scope = h('input', {
    placeholder: 'onvif://www.onvif.org/location/Lab',
    'aria-label': 'Scope URI',
  });
  const add = (uri, message) => act({ action: 'add_scope', scope: uri }, message, result);
  const locationUri = () => 'onvif://www.onvif.org/location/' + encodeURIComponent(location.value);

  // Fixed scopes are built into the camera; only configurable ones can be removed.
  const scopeRow = (entry) =>
    h(
      'tr',
      {},
      h('td', {}, pill(entry.fixed ? 'fixed' : 'configurable', entry.fixed ? '' : 'good')),
      h('td', { class: 'mono', text: decodeURIComponent(entry.uri) }),
      h(
        'td',
        {},
        entry.fixed
          ? null
          : button(
              'Remove',
              () => act({ action: 'remove_scope', scope: entry.uri }, 'Scope removed', result),
              'small danger',
            ),
      ),
    );

  return wideCard(
    'Scopes',
    section(view.scopes, (list) => table(null, list.map(scopeRow))),
    needs(
      view.scopes,
      'scopes',
      row(
        location,
        button('Add location', () => add(locationUri(), 'Location added')),
      ),
    ),
    needs(
      view.scopes,
      'scopes',
      row(
        scope,
        button('Add scope', () => add(scope.value, 'Scope added')),
      ),
    ),
    result,
  );
}

// ---- Maintenance ------------------------------------------------------------

function maintenanceCard() {
  const result = h('div', {});
  const reboot = async () => {
    if (!confirm('Reboot the camera now?')) return;
    const body = { action: 'reboot', confirm: true };
    const reply = await api('POST', base() + '/device', body, { quiet: true });
    result.replaceChildren(
      reply.ok
        ? goodNote('The camera answered: ' + (reply.data || 'rebooting'))
        : errorBox(reply.error),
    );
  };
  return card(
    'Maintenance',
    h('p', { class: 'muted', text: 'Restarting takes the camera offline for about a minute.' }),
    button('Reboot camera', reboot, 'danger'),
    result,
  );
}
