/**
 * @file tabs/check.js
 * @brief The Compatibility tab: run every read-only check and export a report.
 *
 * The server runs the check in the background; this tab polls for results and
 * groups them by area. The JSON report and Markdown summary are meant for bug
 * reports, so they include the camera identity and the library version.
 */

import { api } from '../api.js';
import { card, copy, download, h, row, table, wideCard } from '../dom.js';
import { errorBox, errorSummary } from '../errors.js';
import { base, tabTimer, ui } from '../state.js';

const statusLabels = {
  pass: '✔ pass',
  fail: '✘ fail',
  unsupported: '– unsupported',
  info: 'ℹ note',
};

export async function renderCheck(fill, current) {
  const run = h('button', { class: 'primary', type: 'button', text: 'Run compatibility check' });
  const exportJson = h('button', { type: 'button', text: 'Download JSON report', disabled: true });
  const copyMarkdown = h('button', {
    type: 'button',
    text: 'Copy Markdown summary',
    disabled: true,
  });
  const status = h('div', { class: 'muted' });
  const results = h('div', {});
  let report = null;

  fill(
    wideCard(
      'Compatibility check',
      h(
        'p',
        { class: 'muted' },
        'Runs every read-only operation wlonvif supports against this camera and records how ' +
          'each one went. Nothing on the camera is changed; event and stream checks open and ' +
          'close a subscription and a video stream.',
      ),
      row(run, exportJson, copyMarkdown),
      status,
    ),
    results,
  );

  const draw = (data) => {
    report = data;
    status.replaceChildren(summary(data));
    exportJson.disabled = copyMarkdown.disabled = data.running || !data.results.length;
    run.disabled = data.running;
    // One card per area, in the order the check reached them.
    const areas = [...new Set(data.results.map((result) => result.area))];
    results.replaceChildren(
      ...areas.map((area) =>
        card(
          area,
          table(null, data.results.filter((result) => result.area === area).map(resultRow)),
        ),
      ),
    );
  };

  const poll = async () => {
    const reply = await api('GET', base() + '/check', undefined, { quiet: true });
    if (!current() || !reply.ok) return;
    draw(reply.data);
    if (!reply.data.running) ui.timers.forEach(clearInterval);
  };

  run.onclick = async () => {
    const reply = await api('POST', base() + '/check', {});
    if (reply.ok) {
      tabTimer(poll, 800);
      poll();
    }
  };
  exportJson.onclick = () => {
    const name = (report.camera.identity?.model || report.camera.address).replace(/\W+/g, '_');
    download(`wlonvif-report-${name}.json`, JSON.stringify(report, null, 2), 'application/json');
  };
  copyMarkdown.onclick = () => copy(markdown(report));

  // A check may already be running, for example after switching tabs.
  await poll();
  if (report && report.running) tabTimer(poll, 800);
}

/** Progress while running; afterwards, the count of each outcome. */
function summary(data) {
  if (data.running) {
    return h('span', {}, h('span', { class: 'spinner' }), ` Checking… ${data.results.length} done`);
  }
  if (!data.results.length) return 'Not run yet.';
  const counts = { pass: 0, fail: 0, unsupported: 0, info: 0 };
  for (const result of data.results) counts[result.status] = (counts[result.status] || 0) + 1;
  return h(
    'div',
    { class: 'check-summary' },
    h('span', { class: 'status-pass', text: `${counts.pass} passed` }),
    h('span', { class: 'status-unsupported', text: `${counts.unsupported} unsupported` }),
    h('span', { class: 'status-fail', text: `${counts.fail} failed` }),
    h('span', { class: 'status-info', text: `${counts.info} notes` }),
    h('span', { class: 'muted', text: 'started ' + data.started }),
  );
}

/** One check: its outcome, what the camera answered, and the full error if any. */
function resultRow(result) {
  const error = result.error
    ? h('details', {}, h('summary', { text: errorSummary(result.error) }), errorBox(result.error))
    : null;
  return h(
    'tr',
    {},
    h('td', { class: 'status-' + result.status, text: statusLabels[result.status] }),
    h(
      'td',
      {},
      h('div', { text: result.check }),
      result.detail ? h('div', { class: 'muted small mono', text: result.detail }) : null,
      error,
    ),
    h('td', { class: 'muted small', text: result.ms ? result.ms + ' ms' : '' }),
  );
}

/** The report as a Markdown table, ready to paste into an issue. */
function markdown(report) {
  const identity = report.camera.identity || {};
  const model = identity.model || report.camera.address;
  const cell = (text) => text.replace(/\|/g, '\\|');
  const lines = [
    `### wlonvif ${report.library} compatibility: ${identity.manufacturer || ''} ${model}`,
    `Firmware: ${identity.firmware || 'unknown'}`,
    '',
    '| Area | Check | Result | Detail |',
    '|---|---|---|---|',
  ];
  for (const result of report.results) {
    const detail = result.detail || (result.error ? errorSummary(result.error) : '');
    lines.push(`| ${result.area} | ${result.check} | ${result.status} | ${cell(detail)} |`);
  }
  return lines.join('\n');
}
