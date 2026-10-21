/**
 * @file state.js
 * @brief Page state shared by every tab, and the media profile helpers several tabs use.
 */

import { api } from './api.js';
import { h } from './dom.js';

/** Everything the page remembers; modules read and update it directly. */
export const ui = {
  cameras: [], // From /api/state: the sidebar list.
  selected: null, // Id of the camera shown, such as 'camera-1'.
  tab: 'overview', // Id of the tab shown.
  ffmpeg: false, // Whether the explorer host can show live video.
  recordings: false, // Whether it can record.
  log: [], // Request log entries, newest first.
  lastLog: 0, // Sequence number of the newest entry fetched.
  unseenFailures: 0, // Failures since the drawer was last opened.
  logOpen: false,
  render: 0, // Increments on every tab render, so stale results are dropped.
  timers: [], // Intervals owned by the current tab.
  cleanups: [], // Functions run when the current tab is left.
  media: {}, // Cached media view per camera, used by several tabs.
};

/** The API path of the selected camera. */
export const base = () => '/api/cameras/' + encodeURIComponent(ui.selected);
/** The selected camera's sidebar entry. */
export const camera = () => ui.cameras.find((item) => item.id === ui.selected);
/** setInterval() that stops when the tab is left. */
export const tabTimer = (fn, ms) => ui.timers.push(setInterval(fn, ms));

/** The best name available: model once signed in, else the scan's name or the address. */
export function cameraTitle(item) {
  return item.identity
    ? `${item.identity.manufacturer} ${item.identity.model}`
    : item.name || item.hardware || item.address;
}

/**
 * The selected camera's media view (profiles, URIs, encoder options), cached
 * because several tabs need it; @p force fetches it again.
 */
export async function mediaView(force) {
  if (!force && ui.media[ui.selected]) return ui.media[ui.selected];
  const reply = await api('GET', base() + '/media');
  if (reply.ok) ui.media[ui.selected] = reply.data;
  return reply.ok ? reply.data : null;
}

/** The profiles in a media view, or none if the camera would not list them. */
export function profilesOf(media) {
  return media && media.profiles && media.profiles.ok ? media.profiles.value : [];
}

// Live video starts on the smallest H.264/H.265 stream: it reaches the browser
// with the least delay and costs the least to decode, transcode, and transfer.
// JPEG profiles come last: over RTSP they are often damaged (one
// tested camera's decodes mostly as green frames) and usually run at a token frame rate.
export function previewProfile(profiles) {
  const cost = (profile) => {
    const encoder = profile.video_encoder;
    if (!encoder) return Infinity;
    return (encoder.encoding === 'JPEG' ? 1e9 : 0) + encoder.width * encoder.height;
  };
  return profiles.reduce(
    (best, profile) => (cost(profile) < cost(best) ? profile : best),
    profiles[0],
  ).token;
}

/** A list of profiles that calls @p onchange with the chosen token. */
export function profileSelect(profiles, onchange, selected) {
  const select = h(
    'select',
    { 'aria-label': 'Media profile', onchange: () => onchange && onchange(select.value) },
    profiles.map((profile) =>
      h('option', {
        value: profile.token,
        text: `${profile.name || profile.token} (${profile.token})`,
      }),
    ),
  );
  if (selected) select.value = selected;
  return select;
}
