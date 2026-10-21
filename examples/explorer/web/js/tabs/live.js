/**
 * @file tabs/live.js
 * @brief The Live & recording tab: live video, snapshots, recordings, and stream addresses.
 *
 * Browsers cannot play RTSP, so the explorer host runs FFmpeg and sends the
 * video as MJPEG. Snapshots use the camera's ONVIF snapshot URI when it has one,
 * and otherwise a frame from the stream. Recordings copy the stream into a file
 * on the explorer host without re-encoding.
 */

import { api } from '../api.js';
import { card, facts, formatBytes, h, missing, note, pill, row } from '../dom.js';
import { setUnavailable, spinner, table, toast, wideCard } from '../dom.js';
import { errorBox, section } from '../errors.js';
import {
  base,
  mediaView,
  previewProfile,
  profileSelect,
  profilesOf,
  tabTimer,
  ui,
} from '../state.js';

export async function renderLive(fill, current) {
  const media = await mediaView();
  if (!current()) return;
  const profiles = profilesOf(media);
  if (!profiles.length) {
    return fill(
      media
        ? errorBox(media.profiles.error, 'No media profiles')
        : note('Media profiles unavailable'),
    );
  }
  let profile = previewProfile(profiles);

  // ---- Controls ----------------------------------------------------------------

  const select = profileSelect(
    profiles,
    (value) => {
      profile = value;
      showProfile();
    },
    profile,
  );
  const start = h('button', { class: 'primary', type: 'button', text: 'Start live view' });
  const stop = h('button', { type: 'button', text: 'Stop' });
  const snapshotButton = h('button', { type: 'button', text: 'Take snapshot' });
  const seconds = h('input', {
    type: 'number',
    min: 1,
    max: 3600,
    value: 30,
    'aria-label': 'Recording length in seconds',
  });
  const recordButton = h('button', { class: 'primary', type: 'button', text: 'Record' });

  const image = h('img', { alt: 'Live camera video' });
  const screen = h(
    'div',
    { class: 'video' },
    h('span', {
      text: ui.ffmpeg ? 'Press Start to watch' : 'Live video needs FFmpeg on the explorer host',
    }),
  );
  const liveState = h('div', {});
  const snapshotSource = h('p', { class: 'muted small' });
  const snapshotArea = h('div', {});
  const recordingList = h('div', {});
  const addresses = h('div', {});

  /**
   * Enable what the selected profile allows: its stream and snapshot URIs, and
   * what this explorer host can do with them (FFmpeg, a recordings directory).
   */
  const updateControls = () => {
    const entry = media.uris[profile] || {};
    const stream = entry.stream?.ok && entry.stream.value;
    const snapshot = entry.snapshot?.ok && entry.snapshot.value;
    const noStream =
      entry.stream && !stream ? 'The camera gives no stream URI for this profile.' : null;
    const canGrab = ui.ffmpeg && stream;

    setUnavailable(!ui.ffmpeg ? 'Live video needs FFmpeg on the explorer host.' : noStream, start);
    setUnavailable(
      !ui.recordings ? 'Recording needs FFmpeg and a --recordings directory.' : noStream,
      recordButton,
      seconds,
    );
    setUnavailable(
      snapshot || canGrab
        ? null
        : 'The camera has no snapshot URI, and grabbing a frame from the stream needs FFmpeg.',
      snapshotButton,
    );
    snapshotSource.replaceChildren(
      snapshot
        ? "Uses the camera's ONVIF snapshot URI."
        : missing(
            'The camera offers no snapshot URI for this profile' +
              (canGrab ? '; a frame is taken from the stream instead.' : '.'),
          ),
    );
  };

  const showProfile = () => {
    updateControls();
    addresses.replaceChildren(streamAddresses(media.uris[profile] || {}));
  };

  // ---- Live view ---------------------------------------------------------------

  /** Shown a few seconds after starting: FFmpeg's own words if the stream died. */
  const watch = async () => {
    const status = await api('GET', base() + '/live', undefined, { quiet: true });
    if (!current() || !status.ok) return;
    const data = status.data;
    if (!data.running && data.diagnostics) {
      liveState.replaceChildren(
        errorBox({
          category: 'network',
          operation: 'Live video (FFmpeg)',
          message: 'The stream stopped. FFmpeg reported:\n' + data.diagnostics,
        }),
      );
    } else if (data.running) {
      liveState.replaceChildren(
        h('div', {
          class: 'muted small',
          text: `Streaming profile ${data.profile} through FFmpeg as MJPEG`,
        }),
      );
    } else {
      liveState.replaceChildren();
    }
  };

  start.onclick = async () => {
    start.disabled = true;
    screen.replaceChildren(spinner('Starting the stream…'));
    const reply = await api('POST', base() + '/live/start', { profile });
    updateControls();
    if (!reply.ok) {
      screen.replaceChildren(h('span', { text: 'Not started' }));
      liveState.replaceChildren(errorBox(reply.error));
      return;
    }
    // The timestamp makes the browser open a new stream rather than reuse one.
    image.src = `${base()}/live.mjpeg?profile=${encodeURIComponent(profile)}&t=${Date.now()}`;
    screen.replaceChildren(image);
    setTimeout(watch, 4000);
  };

  stop.onclick = async () => {
    image.removeAttribute('src');
    screen.replaceChildren(h('span', { text: 'Stopped' }));
    await api('POST', base() + '/live/stop', {});
  };

  // ---- Snapshot ----------------------------------------------------------------

  snapshotButton.onclick = async () => {
    snapshotButton.disabled = true;
    snapshotArea.replaceChildren(spinner('Taking a snapshot…'));
    snapshotArea.replaceChildren(await takeSnapshot(profile));
    updateControls();
  };

  // ---- Recordings --------------------------------------------------------------

  const refreshRecordings = async () => {
    const reply = await api('GET', '/api/recordings', undefined, { quiet: true });
    if (!current() || !reply.ok) return;
    recordingList.replaceChildren(
      reply.data.length
        ? table(
            null,
            reply.data.map((item) => recordingRow(item, refreshRecordings)),
          )
        : h('p', { class: 'muted', text: 'No recordings yet.' }),
    );
  };

  recordButton.onclick = async () => {
    const reply = await api('POST', base() + '/record', {
      profile,
      seconds: Number(seconds.value),
    });
    if (reply.ok) {
      toast('Recording to ' + reply.data, 'good');
      refreshRecordings();
    }
  };

  // ---- Layout ------------------------------------------------------------------

  showProfile();
  fill(
    h(
      'div',
      { class: 'grid' },
      wideCard(
        'Live view',
        row(h('label', {}, 'Profile', select), start, stop, snapshotButton),
        row(screen),
        liveState,
      ),
      card('Snapshot', snapshotSource, snapshotArea),
      card(
        'Record (DVR)',
        h(
          'p',
          { class: 'muted small' },
          "Records the selected profile's stream to a file on the explorer host without " +
            're-encoding. ONVIF Profile G on-camera recording is not part of wlonvif; the ' +
            'Overview tab shows whether the camera offers it.',
        ),
        ui.recordings ? null : note('Recording needs FFmpeg and a --recordings directory.'),
        row(h('label', {}, 'Seconds', seconds), recordButton),
        recordingList,
      ),
      wideCard('Stream addresses', addresses),
    ),
  );
  refreshRecordings();
  tabTimer(refreshRecordings, 3000);
  tabTimer(watch, 5000);
}

/** Fetch a snapshot and return what to show: the image with its source, or the error. */
async function takeSnapshot(profile) {
  try {
    const response = await fetch(`${base()}/snapshot?profile=${encodeURIComponent(profile)}`);
    if (!(response.headers.get('Content-Type') || '').startsWith('image/')) {
      const reply = await response.json();
      return errorBox(reply.error);
    }
    const url = URL.createObjectURL(await response.blob());
    const fromOnvif = response.headers.get('X-Snapshot-Source') === 'onvif';
    return h(
      'div',
      {},
      h('div', { class: 'video' }, h('img', { src: url, alt: 'Snapshot' })),
      h(
        'div',
        { class: 'row' },
        h('span', {
          class: 'muted small',
          text: fromOnvif
            ? "From the camera's ONVIF snapshot URI"
            : 'Grabbed from the video stream (the camera offers no usable snapshot URI)',
        }),
        h('a', { href: url, download: `snapshot-${Date.now()}.jpg`, text: 'Download' }),
      ),
    );
  } catch (failure) {
    return errorBox({ category: 'network', message: failure.message });
  }
}

/** One recording: name, size, state, and its actions. */
function recordingRow(item, refresh) {
  const path = `/api/recordings/${encodeURIComponent(item.name)}`;
  const stop = async () => {
    await api('POST', `${path}/stop`, {});
    refresh();
  };
  const remove = async () => {
    if (!confirm(`Delete ${item.name}?`)) return;
    await api('POST', `${path}/delete`, {});
    refresh();
  };
  // A failed recording keeps FFmpeg's log, folded under its name.
  const log = item.log
    ? h(
        'details',
        {},
        h('summary', { class: 'bad', text: 'FFmpeg reported a problem' }),
        h('pre', { text: item.log }),
      )
    : null;
  const state = item.active
    ? h('span', {}, h('span', { class: 'spinner' }), ' recording')
    : pill('done', 'good');
  return h(
    'tr',
    {},
    h('td', {}, h('div', { class: 'mono', text: item.name }), log),
    h('td', { class: 'muted', text: formatBytes(item.bytes) }),
    h('td', {}, state),
    h(
      'td',
      {},
      h(
        'div',
        { class: 'row' },
        item.active
          ? h('button', { class: 'small', type: 'button', text: 'Stop', onclick: stop })
          : null,
        h('a', { href: `${path}/download`, download: item.name, text: 'Download' }),
        h('button', { class: 'small danger', type: 'button', text: 'Delete', onclick: remove }),
      ),
    ),
  );
}

/** The profile's stream and snapshot URIs, and a command line to play the stream. */
function streamAddresses(entry) {
  const mono = (text) => h('span', { class: 'mono', text });
  const stream = entry.stream?.ok ? entry.stream.value : null;
  return h(
    'div',
    {},
    facts([
      ['Stream URI', section(entry.stream, mono)],
      [
        'Snapshot URI',
        section(entry.snapshot, (uri) =>
          uri ? mono(uri) : h('span', { class: 'muted', text: 'empty' }),
        ),
      ],
      ['Play with', stream ? mono(`ffplay -rtsp_transport tcp '${stream}'`) : null],
    ]),
    h('p', {
      class: 'muted small',
      text: 'Players need the camera credentials added to the URI (rtsp://user:password@…).',
    }),
  );
}
