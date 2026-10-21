/**
 * @file tab-bar.js
 * @brief The tab bar, and switching between tabs for the selected camera.
 *
 * Each tab is a function `render(fill, current)` in tabs/. It loads what it
 * needs and calls `fill(...nodes)` to show it. Results can arrive after the
 * user has moved on, so `current()` says whether the tab is still the one
 * shown, and `fill` ignores calls for a tab that was left.
 */

import { $, card, h, missing, row, spinner } from './dom.js';
import { camera, ui } from './state.js';
import { renderAnalytics } from './tabs/analytics.js';
import { renderCheck } from './tabs/check.js';
import { renderDevice } from './tabs/device.js';
import { renderEvents } from './tabs/events.js';
import { renderImaging } from './tabs/imaging.js';
import { renderLive } from './tabs/live.js';
import { renderMedia } from './tabs/media.js';
import { renderOverview } from './tabs/overview.js';
import { renderPtz, stopMove } from './tabs/ptz.js';
import { renderRaw } from './tabs/raw.js';

/** [id used in the URL fragment, label, render function]. */
export const tabs = [
  ['overview', 'Overview', renderOverview],
  ['check', 'Compatibility', renderCheck],
  ['live', 'Live & recording', renderLive],
  ['media', 'Media', renderMedia],
  ['ptz', 'PTZ', renderPtz],
  ['imaging', 'Imaging', renderImaging],
  ['analytics', 'Analytics', renderAnalytics],
  ['events', 'Events', renderEvents],
  ['device', 'Device', renderDevice],
  ['raw', 'Raw request', renderRaw],
];

/** The ONVIF service each tab needs, and its name for messages. */
const tabServices = {
  live: 'media',
  media: 'media',
  ptz: 'ptz',
  imaging: 'imaging',
  analytics: 'analytics',
  events: 'events',
};
const serviceNames = {
  media: 'Media',
  ptz: 'PTZ',
  imaging: 'Imaging',
  analytics: 'Analytics',
  events: 'Event',
};

/** Why a tab cannot work on the selected camera, or null; unknown services count as present. */
function tabMissing(id) {
  const service = tabServices[id];
  const services = camera()?.connected ? camera().services : null;
  return service && services && services[service] === false
    ? `The camera does not advertise an ONVIF ${serviceNames[service]} service.`
    : null;
}

/** Draw the tab bar, dimming tabs the camera cannot serve, and show the current tab. */
export function renderTabs() {
  $('#tabs').replaceChildren(
    ...tabs.map(([id, label]) => {
      const reason = tabMissing(id);
      return h('button', {
        type: 'button',
        role: 'tab',
        'aria-selected': String(id === ui.tab),
        text: label,
        class: reason ? 'unavailable' : null,
        title: reason,
        onclick: () => {
          ui.tab = id;
          history.replaceState(null, '', '#' + ui.selected + '/' + id);
          renderTabs();
        },
      });
    }),
  );
  showTab();
}

/** (Re)draw the current tab, first stopping everything the previous one started. */
export function showTab() {
  ui.timers.forEach(clearInterval);
  ui.timers = [];
  ui.cleanups.forEach((cleanup) => cleanup());
  ui.cleanups = [];
  stopMove();

  const token = ++ui.render;
  const content = $('#tab-content');
  const item = camera();
  if (!item) return;
  if (!item.connected && ui.tab !== 'raw') {
    content.replaceChildren(
      card(
        'Sign in first',
        h(
          'p',
          {},
          "Enter the camera's username and password above and press Connect. " +
            'An empty username tries anonymous access.',
        ),
      ),
    );
    return;
  }

  const render = tabs.find(([id]) => id === ui.tab)[2];
  const run = () => {
    content.replaceChildren(spinner());
    render(
      (...nodes) => {
        if (token === ui.render) content.replaceChildren(...nodes);
      },
      () => token === ui.render,
    );
  };

  const reason = tabMissing(ui.tab);
  if (reason) {
    // Still reachable: a camera can offer a service it does not advertise.
    const tryAnyway = h('button', { type: 'button', text: 'Try anyway', onclick: run });
    content.replaceChildren(card('Not available on this camera', missing(reason), row(tryAnyway)));
    return;
  }
  run();
}
