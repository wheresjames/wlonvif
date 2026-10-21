/**
 * @file main.js
 * @brief Page shell: camera sidebar, network scan, adding cameras, sign-in, and startup.
 *
 * The page polls `/api/state` every two seconds; that one call carries the
 * camera list, scan progress, and what the explorer host supports (FFmpeg,
 * recordings). A URL fragment such as `#camera-1/ptz` opens that camera and
 * tab, so a view can be bookmarked or shared with someone on the same host.
 */

import { api } from './api.js';
import { $, h, pill, toast } from './dom.js';
import { errorBox } from './errors.js';
import { clearLog, pollLog, toggleLog } from './log.js';
import { base, camera, cameraTitle, ui } from './state.js';
import { renderTabs, tabs } from './tab-bar.js';
import { stopHeldMovement } from './tabs/ptz.js';

/** Open the camera and tab a fragment such as #camera-1/ptz names, once that camera is known. */
function followHash() {
  const [id, tab] = location.hash.slice(1).split('/');
  if (!id || (id === ui.selected && tab === ui.tab)) return;
  if (!ui.cameras.some((item) => item.id === id)) return;
  if (tabs.some(([name]) => name === tab)) ui.tab = tab;
  selectCamera(id);
}

async function refreshState() {
  const reply = await api('GET', '/api/state', undefined, { quiet: true });
  if (!reply.ok) return;
  const state = reply.data;
  $('#version').textContent = 'library ' + state.library;
  ui.ffmpeg = state.ffmpeg;
  ui.recordings = state.recordings;
  // Offer the server's default username until the user types their own.
  if (!$('#login-user').value && state.default_username && !ui.loginTouched) {
    $('#login-user').value = state.default_username;
  }
  ui.cameras = state.cameras;
  renderSidebar();
  renderScan(state.scan);
  if (ui.selected) renderHeader();
  followHash();
}

// ---- Sidebar ----------------------------------------------------------------

function renderSidebar() {
  $('#camera-list').replaceChildren(...ui.cameras.map((item) => h('li', {}, sidebarEntry(item))));
  $('#camera-empty').hidden = ui.cameras.length > 0;
  $('#camera-count').textContent = ui.cameras.length ? String(ui.cameras.length) : '';
}

/** A camera in the sidebar: name, address, how a scan recognized it, and sign-in state. */
function sidebarEntry(item) {
  const dot = item.connected ? 'on' : item.error ? 'off' : '';
  const evidence = item.evidence
    ? pill(item.evidence, item.evidence === 'confirmed' ? 'good' : 'warn')
    : null;
  return h(
    'button',
    {
      type: 'button',
      class: item.id === ui.selected ? 'selected' : '',
      onclick: () => selectCamera(item.id),
    },
    h('span', { class: 'camera-name' }, h('span', { class: 'dot ' + dot }), cameraTitle(item)),
    h('span', { class: 'muted small' }, item.address, item.location ? ' · ' + item.location : ''),
    h(
      'span',
      { class: 'small' },
      evidence,
      item.connected ? [' ', pill('signed in', 'good')] : null,
    ),
  );
}

// ---- Network scan -----------------------------------------------------------

function renderScan(scan) {
  const running = scan && scan.running;
  $('#scan-button').disabled = running;
  $('#scan-progress').hidden = !running;
  if (running) {
    const percent = scan.total ? Math.round((100 * scan.completed) / scan.total) : 3;
    const stage = scan.stage === 'ws-discovery' ? 'WS-Discovery' : 'Probing';
    $('#scan-bar').style.width = percent + '%';
    $('#scan-text').textContent = `${stage} ${scan.completed}/${scan.total} ${scan.current || ''}`;
  }

  const summary = $('#scan-summary');
  if (!scan || running || !scan.finished) {
    summary.hidden = true;
    return;
  }
  summary.hidden = false;
  const networks = (scan.networks || []).join(', ');
  const parts = [h('div', {}, `Last scan: ${scan.found ?? 0} found on ${networks}`)];
  if (scan.error) parts.push(errorBox(scan.error));
  // Addresses the time limit cut short: a camera there may have been missed.
  const unchecked = (scan.unchecked || []).length;
  const incomplete = (scan.incomplete || []).length;
  if (unchecked || incomplete) {
    parts.push(
      h(
        'div',
        { class: 'warn' },
        `${unchecked} address(es) unchecked, ${incomplete} partly checked before the time limit.`,
      ),
    );
  }
  summary.replaceChildren(...parts);
}

async function startScan() {
  const reply = await api('POST', '/api/scan', {});
  if (reply.ok) {
    toast('Scanning the local networks…');
    renderScan(reply.data);
  }
}

// ---- Selected camera --------------------------------------------------------

function selectCamera(id) {
  ui.selected = id;
  history.replaceState(null, '', '#' + id + '/' + ui.tab);
  ui.tab = ui.tab || 'overview';
  $('#welcome').hidden = true;
  $('#camera').hidden = false;
  $('#login-password').value = '';
  renderSidebar();
  renderHeader();
  renderTabs();
}

/** Title, address, firmware, and sign-in state above the tabs. */
function renderHeader() {
  const item = camera();
  if (!item) {
    // The camera was forgotten: back to the welcome text.
    $('#camera').hidden = true;
    $('#welcome').hidden = false;
    return;
  }
  $('#camera-title').textContent = cameraTitle(item);
  const subtitle = [item.address];
  if (item.identity) subtitle.push('firmware ' + item.identity.firmware);
  if (item.url) subtitle.push(item.url);
  $('#camera-subtitle').textContent = subtitle.join(' · ');

  const state = $('#login-state');
  if (item.connected) {
    state.textContent = 'signed in' + (item.username ? ' as ' + item.username : '');
    state.className = 'pill good';
  } else {
    state.textContent = item.error ? 'sign-in failed' : 'not signed in';
    state.className = 'pill ' + (item.error ? 'bad' : '');
  }
  $('#camera-error').replaceChildren(
    !item.connected && item.error ? errorBox(item.error, 'Sign-in failed') : '',
  );
}

async function connect(event) {
  event.preventDefault();
  const button = event.submitter || $('#login button');
  button.disabled = true;
  const reply = await api('POST', base() + '/connect', {
    username: $('#login-user').value,
    password: $('#login-password').value,
  });
  button.disabled = false;
  await refreshState();
  if (reply.ok) {
    toast('Signed in to ' + cameraTitle(reply.data), 'good');
    renderTabs();
  }
}

/** The Add camera dialog: an address or Device service URL, and optional credentials. */
async function addCamera(event) {
  if (event.submitter?.value !== 'add') return;
  const target = $('#add-target').value.trim();
  const body = target.includes('://') ? { url: target } : { address: target };
  if ($('#add-user').value) {
    body.username = $('#add-user').value;
    body.password = $('#add-password').value;
  }
  const reply = await api('POST', '/api/cameras', body);
  await refreshState();
  if (reply.ok) {
    selectCamera(reply.data.id);
    toast('Camera added', 'good');
  } else {
    // Added, but signing in failed: open it so the error shows.
    const added = ui.cameras.find((item) => item.address === target || item.url === target);
    if (added) selectCamera(added.id);
  }
}

// ---- Startup ----------------------------------------------------------------

$('#scan-button').addEventListener('click', startScan);
$('#add-button').addEventListener('click', () => {
  $('#add-form').reset();
  $('#add-dialog').showModal();
});
$('#add-form').addEventListener('submit', addCamera);
$('#login').addEventListener('submit', connect);
$('#login-user').addEventListener('input', () => {
  ui.loginTouched = true;
});
$('#log-button').addEventListener('click', () => toggleLog());
$('#log-close').addEventListener('click', () => toggleLog(false));
$('#log-clear').addEventListener('click', clearLog);

// Never leave the camera moving when the page loses focus.
window.addEventListener('blur', stopHeldMovement);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) stopHeldMovement();
});

window.addEventListener('hashchange', followHash);
refreshState().then(followHash);
pollLog();
setInterval(refreshState, 2000);
setInterval(pollLog, 2000);
