/**
 * @file tabs/media.js
 * @brief The Media tab: profiles, video sources, and an editor per video encoder.
 *
 * Encoder fields are limited to the ranges the camera reports, and a field the
 * camera fixes to one value is disabled. After a change the server reads the
 * encoder back and names every field the camera kept at its own value.
 */

import { api } from '../api.js';
import { card, facts, goodNote, h, note, row, setUnavailable, table, wideCard } from '../dom.js';
import { errorBox, problem, section } from '../errors.js';
import { base, mediaView, profilesOf, ui } from '../state.js';

export async function renderMedia(fill, current) {
  const media = await mediaView(true);
  if (!current()) return;
  if (!media) return fill(note('Media view unavailable'));

  // Profiles often share an encoder; edit each encoder once.
  const encoders = new Map();
  for (const profile of profilesOf(media)) {
    const encoder = profile.video_encoder;
    if (encoder && !encoders.has(encoder.token)) encoders.set(encoder.token, encoder);
  }
  const headings = ['Profile', 'Video source', 'Video encoder', 'Audio', 'PTZ', 'Analytics'];
  fill(
    h(
      'div',
      { class: 'grid' },
      wideCard(
        'Profiles',
        section(media.profiles, (list) => table(headings, list.map(profileRow))),
      ),
      card(
        'Video sources',
        section(media.sources, (sources) =>
          facts(
            sources.map((source) => [
              source.token,
              `${source.width}×${source.height} @ ${source.framerate} fps`,
            ]),
          ),
        ),
      ),
      [...encoders.values()].map((encoder) =>
        encoderEditor(encoder, media.encoder_options[encoder.token]),
      ),
    ),
  );
}

/** One profile: what it combines, each part summarized in a cell. */
function profileRow(profile) {
  const source = profile.video_source;
  const video = profile.video_encoder;
  const audio = profile.audio_encoder;
  const cell = (text, mono) => h('td', { class: mono ? 'small mono' : 'small', text: text || '—' });
  return h(
    'tr',
    {},
    h(
      'td',
      {},
      h('strong', { text: profile.name }),
      h('div', {
        class: 'muted small mono',
        text: profile.token + (profile.fixed ? ' · fixed' : ''),
      }),
    ),
    cell(source && `${source.source} ${source.width}×${source.height}`),
    cell(
      video &&
        `${video.encoding} ${video.width}×${video.height} · ${video.fps} fps · ` +
          `${video.bitrate} kbit/s · quality ${video.quality}`,
    ),
    cell(audio && `${audio.encoding} ${audio.sample_rate}`),
    cell(profile.ptz, true),
    cell(profile.analytics, true),
  );
}

/** Editable fields for one encoder, bounded by the options the camera reports. */
function encoderEditor(encoder, optionsSection) {
  const options = optionsSection?.ok ? optionsSection.value : null;
  // Ranges depend on the encoding: H.264 and JPEG allow different sizes and rates.
  const encoding = options?.encodings.find((item) => item.encoding === encoder.encoding) ?? null;
  const result = h('div', {});

  const number = (value, range, label) =>
    h('input', { type: 'number', value, min: range?.min, max: range?.max, 'aria-label': label });
  const sizes = encoding
    ? encoding.resolutions
    : [{ width: encoder.width, height: encoder.height }];
  const resolution = h(
    'select',
    { 'aria-label': 'Resolution' },
    sizes.map((size) =>
      h('option', {
        value: `${size.width}x${size.height}`,
        text: `${size.width}×${size.height}`,
        selected: size.width === encoder.width && size.height === encoder.height,
      }),
    ),
  );
  const quality = number(encoder.quality, options?.quality, 'Quality');
  const fps = number(encoder.fps, encoding?.fps, 'Frame rate');
  const bitrate = number(encoder.bitrate, encoding?.bitrate, 'Bitrate');
  const gov = number(encoder.gov ?? '', encoding?.gov, 'GOP length');

  const fixed = (range) =>
    range && range.min === range.max ? `The camera allows only ${range.min}.` : null;
  const rangeText = (range) =>
    range ? (range.min === range.max ? `fixed ${range.min}` : `${range.min}–${range.max}`) : '';

  // [field, reason it cannot be changed or null].
  const fields = [
    [
      h('label', {}, 'Resolution', resolution),
      sizes.length < 2 ? 'The camera offers only this resolution.' : null,
    ],
    [h('label', {}, `Quality ${rangeText(options?.quality)}`, quality), fixed(options?.quality)],
    [h('label', {}, `Frame rate ${rangeText(encoding?.fps)}`, fps), fixed(encoding?.fps)],
    [
      h('label', {}, `Bitrate kbit/s ${rangeText(encoding?.bitrate)}`, bitrate),
      fixed(encoding?.bitrate),
    ],
    [
      h('label', {}, `GOP length ${rangeText(encoding?.gov)}`, gov),
      encoder.gov === null ? 'This encoding has no GOP length.' : fixed(encoding?.gov),
    ],
  ];
  for (const [field, reason] of fields) setUnavailable(reason, field);

  const apply = h('button', { class: 'primary', type: 'button', text: 'Apply' });
  apply.onclick = async () => {
    const [width, height] = resolution.value.split('x').map(Number);
    const body = {
      token: encoder.token,
      width,
      height,
      quality: Number(quality.value),
      fps: Number(fps.value),
      bitrate: Number(bitrate.value),
    };
    if (!gov.disabled) body.gov = Number(gov.value);
    apply.disabled = true;
    const reply = await api('POST', base() + '/media/encoder', body, { quiet: true });
    apply.disabled = false;
    if (!reply.ok) return result.replaceChildren(errorBox(reply.error));
    const ignored = reply.data.ignored;
    result.replaceChildren(
      ignored.length
        ? note(`The camera accepted the change but kept its own ${ignored.join(', ')}.`)
        : goodNote('Applied.'),
    );
    delete ui.media[ui.selected]; // Other tabs re-read the profiles.
  };
  if (fields.every(([, reason]) => reason)) {
    setUnavailable('The camera allows no changes to this encoder.', apply);
  }

  const description = `${encoder.encoding}${encoder.profile ? ' ' + encoder.profile : ''} · ${encoder.name}`;
  return card(
    `Encoder ${encoder.token}`,
    h('div', { class: 'muted small', text: description }),
    optionsSection && !optionsSection.ok
      ? problem(optionsSection.error, 'Options unavailable; values are not checked')
      : null,
    h(
      'div',
      { class: 'grid' },
      fields.map(([field]) => field),
    ),
    row(apply),
    result,
  );
}
