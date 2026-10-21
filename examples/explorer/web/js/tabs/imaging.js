/**
 * @file tabs/imaging.js
 * @brief The Imaging tab: brightness, contrast, and the other image settings.
 *
 * One card per video source. Numeric settings are sliders bounded by the
 * camera's reported ranges; mode settings (IR cut filter, white balance, ...)
 * are lists of the modes it offers. A setting the camera neither reports nor
 * offers options for is shown dimmed as not supported. Only changed settings
 * are sent.
 */

import { api } from '../api.js';
import { card, goodNote, h, missing, note, row, setUnavailable } from '../dom.js';
import { errorBox, problem } from '../errors.js';
import { base } from '../state.js';

const numericSettings = [
  ['brightness', 'Brightness'],
  ['contrast', 'Contrast'],
  ['saturation', 'Colour saturation'],
  ['sharpness', 'Sharpness'],
];
const modeSettings = [
  ['ir_cut', 'IR cut filter'],
  ['backlight', 'Backlight compensation'],
  ['wdr', 'Wide dynamic range'],
  ['white_balance', 'White balance'],
];

export async function renderImaging(fill, current) {
  const reply = await api('GET', base() + '/imaging');
  if (!current()) return;
  if (!reply.ok) return fill(problem(reply.error));
  fill(h('div', { class: 'grid' }, reply.data.map(imagingCard)));
}

/** A labelled row: name, control, and an optional value readout. */
const setting = (label, control, readout) =>
  h('div', { class: 'slider' }, h('span', { text: label }), control, readout || h('span', {}));

/** A setting with neither a value nor options: one the camera lacks. */
const absent = (label) => {
  const element = setting(label, missing('not supported'));
  setUnavailable('Not supported by this camera.', element);
  return element;
};

function imagingCard(entry) {
  const source = entry.source;
  const settings = entry.settings.ok ? entry.settings.value : {};
  const options = entry.options.ok ? entry.options.value : {};
  const controls = {}; // key -> { input, initial }
  const rows = [];

  for (const [key, label] of numericSettings) {
    const range = options[key];
    const value = settings[key];
    if (!range && value === null) {
      rows.push(absent(label));
      continue;
    }
    const readout = h('span', { class: 'mono', text: value ?? '—' });
    const input = h('input', {
      type: 'range',
      min: range?.min ?? 0,
      max: range?.max ?? 100,
      step: 1,
      value: value ?? 0,
      disabled: !range, // A value without a range cannot be changed safely.
      'aria-label': label,
      oninput: () => {
        readout.textContent = input.value;
      },
    });
    controls[key] = { input, initial: value };
    rows.push(setting(label, input, readout));
  }

  for (const [key, label] of modeSettings) {
    const modes = options[key] || [];
    const value = settings[key];
    if (!modes.length && value === null) {
      rows.push(absent(label));
      continue;
    }
    const select = h(
      'select',
      { disabled: !modes.length, 'aria-label': label },
      (modes.length ? modes : [value]).map((mode) =>
        h('option', { value: mode, text: mode, selected: mode === value }),
      ),
    );
    controls[key] = { input: select, initial: value };
    rows.push(setting(label, select));
  }

  const result = h('div', {});
  const apply = h('button', { class: 'primary', type: 'button', text: 'Apply changes' });
  apply.onclick = async () => {
    const changed = {};
    for (const [key, { input, initial }] of Object.entries(controls)) {
      const value = input.type === 'range' ? Number(input.value) : input.value;
      if (!input.disabled && value !== initial) changed[key] = value;
    }
    if (!Object.keys(changed).length) return result.replaceChildren(note('Nothing changed.'));
    const body = { source: source.token, settings: changed };
    const reply = await api('POST', base() + '/imaging', body, { quiet: true });
    if (!reply.ok) return result.replaceChildren(errorBox(reply.error));
    for (const [key, value] of Object.entries(changed)) controls[key].initial = value;
    const summary = Object.entries(changed).map(([key, value]) => `${key} ${value}`);
    result.replaceChildren(goodNote('Applied: ' + summary.join(', ')));
  };
  if (!Object.values(controls).some((control) => !control.input.disabled)) {
    setUnavailable('The camera reports no adjustable imaging settings.', apply);
  }

  return card(
    `Video source ${source.token}`,
    h('div', {
      class: 'muted small',
      text: `${source.width}×${source.height} @ ${source.framerate} fps`,
    }),
    entry.settings.ok ? null : problem(entry.settings.error),
    entry.options.ok
      ? null
      : problem(entry.options.error, 'Ranges unavailable; sliders are disabled'),
    rows,
    row(apply),
    result,
  );
}
