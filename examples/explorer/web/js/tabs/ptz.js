/**
 * @file tabs/ptz.js
 * @brief The PTZ tab's movement controls: pad, zoom, keyboard, live video, and position.
 *
 * Holding a control sends an *intent* every 150 ms with the current direction
 * and speed; releasing it sends a zero intent. The server turns intents into
 * camera commands (one command per press in Direct mode) and stops the camera
 * by itself if the intents stop arriving, so a closed tab never leaves it moving.
 *
 * The lower cards (presets, tours, orientation, capabilities) are in ptz-settings.js.
 */

import { api } from '../api.js';
import { h, missing, note, setUnavailable, spinner, toast, wideCard } from '../dom.js';
import { errorBox, errorSummary, problem } from '../errors.js';
import {
  base,
  mediaView,
  previewProfile,
  profileSelect,
  profilesOf,
  tabTimer,
  ui,
} from '../state.js';
import { ptzSettingCards } from './ptz-settings.js';

/** How often a held control repeats its intent; the server's lease is 750 ms. */
const intentInterval = 150;

/** The control being held, if any; shared by the pad, the keyboard, and the page shell. */
const move = { timer: null, vector: null, profile: null, error: null };

/** Stop any held movement, sending Stop to the camera if something was moving. */
export function stopHeldMovement() {
  stopMove(move.vector !== null);
}

/** Direct sends one command per press; smooth ramps and refreshes the velocity. */
function movementMode() {
  try {
    return localStorage.getItem('wlonvif-ptz-mode') === 'smooth' ? 'smooth' : 'direct';
  } catch {
    return 'direct';
  }
}

/** End the held movement. @p sendStop also sends an explicit Stop, as the STOP button does. */
export function stopMove(sendStop) {
  if (move.timer) clearInterval(move.timer);
  const profile = move.profile;
  const wasMoving = move.vector !== null;
  move.timer = null;
  move.vector = null;
  if (profile && (wasMoving || sendStop)) {
    const zero = { profile, pan: 0, tilt: 0, zoom: 0, mode: movementMode() };
    api('POST', base() + '/ptz/intent', zero, { quiet: true });
    if (sendStop) api('POST', base() + '/ptz/stop', { profile });
  }
}

export async function renderPtz(fill, current) {
  const media = await mediaView();
  if (!current()) return;
  const profiles = profilesOf(media).filter((profile) => profile.ptz);
  if (!profiles.length) {
    return fill(
      note(
        'No media profile has a PTZ configuration; this camera cannot pan, tilt, or zoom over ONVIF.',
      ),
    );
  }
  let profile = previewProfile(profiles);
  // What the selected profile can do, from its PTZ configuration options.
  const supports = { panTilt: true, zoom: true };

  // ---- Settings row ----------------------------------------------------------

  const select = profileSelect(
    profiles,
    (value) => {
      stopMove();
      profile = value;
      loadSettings();
    },
    profile,
  );
  const speed = h('input', {
    type: 'range',
    min: 0.1,
    max: 1,
    step: 0.05,
    value: 0.6,
    'aria-label': 'Speed',
  });
  const mode = h(
    'select',
    {
      'aria-label': 'Movement mode',
      title:
        'Direct sends one command per press and suits most cameras; smooth ramps the speed ' +
        'and resends it, which some cameras answer with motor noise.',
      onchange: () => {
        stopMove();
        try {
          localStorage.setItem('wlonvif-ptz-mode', mode.value);
        } catch {
          // Private windows may refuse storage; the choice then lasts for this page only.
        }
      },
    },
    h('option', { value: 'direct', text: 'Direct', selected: movementMode() === 'direct' }),
    h('option', {
      value: 'smooth',
      text: 'Smooth (ramped)',
      selected: movementMode() === 'smooth',
    }),
  );
  const status = h('div', { class: 'muted small' });
  const moveError = h('div', {});

  // ---- Holding a control -----------------------------------------------------

  /** Send the held direction at the current speed; show the first failure only. */
  const intent = () => {
    if (!move.vector) return;
    const s = Number(speed.value);
    const [pan, tilt, zoom] = move.vector.map((axis) => axis * s);
    const body = { profile, pan, tilt, zoom, mode: movementMode() };
    api('POST', base() + '/ptz/intent', body, { quiet: true }).then((reply) => {
      if (!reply.ok && !move.error) {
        move.error = reply.error;
        moveError.replaceChildren(errorBox(reply.error, 'Movement failed'));
      }
    });
  };
  const startMove = (vector) => {
    move.error = null;
    moveError.replaceChildren();
    move.profile = profile;
    move.vector = vector;
    intent();
    move.timer = setInterval(intent, intentInterval);
  };

  /** Make @p button move in @p vector while pressed, by mouse, pen, or touch. */
  const hold = (vector, button) => {
    const end = () => {
      button.classList.remove('active');
      stopMove();
    };
    button.addEventListener('pointerdown', (event) => {
      event.preventDefault();
      // Capture keeps the release coming here even if the pointer slides off.
      button.setPointerCapture?.(event.pointerId);
      button.classList.add('active');
      startMove(vector);
    });
    button.addEventListener('pointerup', end);
    button.addEventListener('pointercancel', end);
    button.addEventListener('lostpointercapture', end);
    return button;
  };
  const padButton = (label, vector, title) =>
    hold(vector, h('button', { type: 'button', text: label, title, 'aria-label': title }));

  // ---- Controls ----------------------------------------------------------------

  // Vectors are [pan, tilt, zoom], each -1, 0, or 1, scaled by the speed slider.
  const pad = h(
    'div',
    { class: 'pad' },
    padButton('↖', [-1, 1, 0], 'Up left'),
    padButton('↑', [0, 1, 0], 'Tilt up'),
    padButton('↗', [1, 1, 0], 'Up right'),
    padButton('←', [-1, 0, 0], 'Pan left'),
    h('button', { class: 'stop', type: 'button', text: 'STOP', onclick: () => stopMove(true) }),
    padButton('→', [1, 0, 0], 'Pan right'),
    padButton('↙', [-1, -1, 0], 'Down left'),
    padButton('↓', [0, -1, 0], 'Tilt down'),
    padButton('↘', [1, -1, 0], 'Down right'),
  );
  const directionButtons = [...pad.querySelectorAll('button:not(.stop)')];
  const zoom = h(
    'div',
    { class: 'row' },
    padButton('Zoom +', [0, 0, 1], 'Zoom in'),
    padButton('Zoom −', [0, 0, -1], 'Zoom out'),
  );
  const relative = (pan, tilt) => api('POST', base() + '/ptz/relative', { profile, pan, tilt });
  const nudge = h(
    'div',
    { class: 'row' },
    h('button', {
      class: 'small',
      type: 'button',
      text: 'Nudge left',
      onclick: () => relative(-0.05, 0),
    }),
    h('button', {
      class: 'small',
      type: 'button',
      text: 'Nudge right',
      onclick: () => relative(0.05, 0),
    }),
  );
  const capabilityNote = h('div', {});

  /** Dim the controls the profile cannot use; null spaces (unknown) leave all enabled. */
  const applySpaces = (spaces) => {
    supports.panTilt = !spaces || spaces.continuous;
    supports.zoom = !spaces || spaces.continuous_zoom;
    const noPanTilt = supports.panTilt ? null : 'This profile has no continuous pan/tilt.';
    const noZoom = supports.zoom ? null : 'The camera reports no zoom for this profile.';
    const noRelative = !spaces || spaces.relative ? null : 'This profile has no relative pan/tilt.';
    setUnavailable(noPanTilt, directionButtons);
    setUnavailable(noZoom, zoom);
    setUnavailable(noRelative, nudge);
    capabilityNote.replaceChildren(
      noPanTilt ? missing('No continuous pan/tilt on this profile.') : '',
      noZoom ? missing('No zoom: the camera reports no zoom movement for this profile.') : '',
    );
  };

  // ---- Keyboard ------------------------------------------------------------------

  const keys = {
    ArrowUp: [0, 1, 0],
    ArrowDown: [0, -1, 0],
    ArrowLeft: [-1, 0, 0],
    ArrowRight: [1, 0, 0],
    '+': [0, 0, 1],
    '-': [0, 0, -1],
  };
  const typing = () => ['INPUT', 'SELECT', 'TEXTAREA'].includes(document.activeElement?.tagName);
  const keydown = (event) => {
    if (ui.tab !== 'ptz' || typing()) return;
    if (event.key === ' ') {
      event.preventDefault();
      stopMove(true);
      return;
    }
    const vector = keys[event.key];
    // Key repeat fires keydown again while held; only the first one starts a move.
    if (!vector || move.vector) return;
    if (vector[2] ? !supports.zoom : !supports.panTilt) return;
    event.preventDefault();
    startMove(vector);
  };
  const keyup = (event) => {
    if (keys[event.key]) stopMove();
  };
  document.addEventListener('keydown', keydown);
  document.addEventListener('keyup', keyup);
  ui.cleanups.push(() => {
    document.removeEventListener('keydown', keydown);
    document.removeEventListener('keyup', keyup);
  });

  // ---- Position and settings ---------------------------------------------------

  const viewPath = () => `${base()}/ptz?profile=${encodeURIComponent(profile)}`;

  const refreshStatus = async () => {
    const reply = await api('GET', viewPath(), undefined, { quiet: true });
    if (!current() || !reply.ok || !reply.data.status.ok) return;
    const { position, moving } = reply.data.status.value;
    const value = (number) => (number === null ? '—' : number.toFixed(3));
    const zoomText = position.zoom === null ? '' : ` · zoom ${value(position.zoom)}`;
    status.textContent =
      `pan ${value(position.pan)} · tilt ${value(position.tilt)}${zoomText} · ` +
      (moving ? 'moving' : 'idle');
  };

  const holder = h('div', { class: 'span' });
  const loadSettings = async () => {
    holder.replaceChildren(spinner());
    const reply = await api('GET', viewPath());
    if (!current()) return;
    if (!reply.ok) return holder.replaceChildren(problem(reply.error));
    const view = reply.data;
    applySpaces(view.spaces.ok ? view.spaces.value : null);
    const after = async (result, message) => {
      if (result.ok) {
        toast(message, 'good');
        loadSettings();
      }
    };
    holder.replaceChildren(ptzSettingCards(view, { profile, after }));
  };

  // ---- Layout ------------------------------------------------------------------

  const live = h(
    'div',
    { class: 'video' },
    h('span', { text: ui.ffmpeg ? 'Video starts with the controls' : 'Video needs FFmpeg' }),
  );
  fill(
    h(
      'div',
      { class: 'grid' },
      wideCard(
        'Control',
        h(
          'div',
          { class: 'row' },
          h('label', {}, 'Profile', select),
          h('label', {}, 'Movement', mode),
          h('label', {}, 'Speed', speed),
          status,
        ),
        h(
          'div',
          { class: 'grid' },
          live,
          h(
            'div',
            {},
            pad,
            zoom,
            nudge,
            capabilityNote,
            h('p', {
              class: 'muted small',
              text:
                'Hold a button, or use the arrow keys and +/−. Space stops. ' +
                'Movement stops by itself if this page stops sending.',
            }),
            moveError,
          ),
        ),
      ),
      holder,
    ),
  );

  // Live video beside the controls, so the movement can be seen.
  if (ui.ffmpeg) {
    const reply = await api('POST', base() + '/live/start', { profile }, { quiet: true });
    if (current() && reply.ok) {
      const src = `${base()}/live.mjpeg?profile=${encodeURIComponent(profile)}&t=${Date.now()}`;
      live.replaceChildren(h('img', { alt: 'Live camera video', src }));
    } else if (current()) {
      live.replaceChildren(h('span', { text: 'Video unavailable: ' + errorSummary(reply.error) }));
    }
  }
  loadSettings();
  refreshStatus();
  tabTimer(refreshStatus, 2000);
}
