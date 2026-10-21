/**
 * @file log.js
 * @brief The request log drawer: every ONVIF exchange the explorer made, newest first.
 *
 * The server keeps the last 500 entries; the page fetches new ones every two
 * seconds. A badge counts failures that arrived while the drawer was closed,
 * and clicking a failed row unfolds its full error.
 */

import { api } from './api.js';
import { $, h, pill } from './dom.js';
import { errorBox, errorSummary } from './errors.js';
import { cameraTitle, ui } from './state.js';

/** Fetch entries newer than the last one seen. */
export async function pollLog() {
  const reply = await api('GET', '/api/log?since=' + ui.lastLog, undefined, { quiet: true });
  if (!reply.ok || !reply.data.length) return;
  for (const entry of reply.data) {
    ui.lastLog = Math.max(ui.lastLog, entry.sequence);
    ui.log.unshift(entry);
    if (!entry.ok && !ui.logOpen) ui.unseenFailures++;
  }
  ui.log.length = Math.min(ui.log.length, 500);
  const badge = $('#log-badge');
  badge.hidden = !ui.unseenFailures;
  badge.textContent = ui.unseenFailures ? String(ui.unseenFailures) : '';
  if (ui.logOpen) renderLog();
}

export function renderLog() {
  const names = Object.fromEntries(ui.cameras.map((item) => [item.id, cameraTitle(item)]));
  $('#log-rows').replaceChildren(...ui.log.map((entry) => logRow(entry, names)));
}

/** Open or close the drawer; without @p open, toggle it. */
export function toggleLog(open) {
  ui.logOpen = open ?? !ui.logOpen;
  $('#log-drawer').hidden = !ui.logOpen;
  $('#log-button').setAttribute('aria-expanded', String(ui.logOpen));
  if (ui.logOpen) {
    ui.unseenFailures = 0;
    $('#log-badge').hidden = true;
    renderLog();
  }
}

export function clearLog() {
  ui.log = [];
  renderLog();
}

function logRow(entry, names) {
  const result = entry.ok
    ? pill('ok', 'good')
    : h('span', { class: 'bad', text: errorSummary(entry.error) });
  const row = h(
    'tr',
    { class: entry.ok ? '' : 'failed' },
    h('td', { class: 'mono', text: entry.time.slice(11, 19) }), // HH:MM:SS
    h('td', { text: names[entry.camera] || entry.camera || '—' }),
    h('td', { text: entry.action }),
    h('td', { class: 'muted', text: entry.ms }),
    h('td', {}, result),
  );
  if (!entry.ok) {
    // Click to show or hide the full error under the row.
    row.addEventListener('click', () => {
      const next = row.nextElementSibling;
      if (next && next.classList.contains('detail')) return next.remove();
      row.after(h('tr', { class: 'detail' }, h('td', { colspan: 5 }, errorBox(entry.error))));
    });
  }
  return row;
}
