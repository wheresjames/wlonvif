/**
 * @file tabs/ptz-settings.js
 * @brief The PTZ tab's lower cards: presets, preset tours, orientation, and capabilities.
 *
 * Each card is built from the PTZ view the server returns for one profile
 * (`GET /api/cameras/ID/ptz?profile=TOKEN`). Actions call `context.after()`,
 * which shows a toast and reloads the cards so they reflect the camera's state.
 */

import { api } from '../api.js';
import { card, facts, goodNote, h, note, pill, row, setUnavailable, table } from '../dom.js';
import { problem, section, sectionUnsupported } from '../errors.js';
import { base } from '../state.js';

/**
 * The cards for one profile.
 *
 * @param view     The server's PTZ view: spaces, status, presets, tours, configuration.
 * @param context  `{ profile, after(reply, message) }`.
 */
export function ptzSettingCards(view, context) {
  return h(
    'div',
    { class: 'grid' },
    presetsCard(view, context),
    toursCard(view, context),
    orientationCard(view, context),
    capabilitiesCard(view),
  );
}

/** POST a PTZ action for the context's profile. */
function ptzPost(context, action, body, options) {
  return api('POST', `${base()}/ptz/${action}`, { profile: context.profile, ...body }, options);
}

/** A small button that runs a PTZ action, then reloads the cards with @p message. */
function actionButton(context, label, action, body, message, tone) {
  return h('button', {
    class: tone ? `small ${tone}` : 'small',
    type: 'button',
    text: label,
    onclick: async () => context.after(await ptzPost(context, action, body), message),
  });
}

// ---- Presets ----------------------------------------------------------------

function presetsCard(view, context) {
  const name = h('input', { placeholder: 'Preset name', 'aria-label': 'Preset name' });
  const save = h(
    'div',
    { class: 'row' },
    name,
    h('button', {
      type: 'button',
      text: 'Save current position',
      onclick: async () =>
        context.after(
          await ptzPost(context, 'preset', { operation: 'save', name: name.value || 'Preset' }),
          'Preset saved',
        ),
    }),
  );
  if (sectionUnsupported(view.presets)) {
    setUnavailable('The camera does not support presets.', save);
  }
  const list = section(view.presets, (presets) =>
    presets.length
      ? table(
          null,
          presets.map((preset) => presetRow(preset, context)),
        )
      : h('p', { class: 'muted', text: 'No presets saved.' }),
  );
  return card('Presets', list, save);
}

function presetRow(preset, context) {
  const label = preset.name || preset.token;
  const token = preset.token;
  return h(
    'tr',
    {},
    h(
      'td',
      {},
      h('strong', { text: preset.name || '(unnamed)' }),
      h('div', { class: 'muted small mono', text: token }),
    ),
    h(
      'td',
      {},
      h(
        'div',
        { class: 'row' },
        actionButton(
          context,
          'Go to',
          'preset',
          { operation: 'goto', token },
          `Moving to ${label}`,
        ),
        // Saving with an existing token moves that preset to the current position.
        actionButton(
          context,
          'Update here',
          'preset',
          { operation: 'save', token, name: preset.name },
          'Preset updated',
        ),
        actionButton(
          context,
          'Remove',
          'preset',
          { operation: 'remove', token },
          'Preset removed',
          'danger',
        ),
      ),
    ),
  );
}

// ---- Preset tours -----------------------------------------------------------

function toursCard(view, context) {
  const list = section(view.tours, (tours) =>
    tours.length
      ? table(
          null,
          tours.map((tour) => tourRow(tour, context)),
        )
      : h('p', { class: 'muted', text: 'No tours.' }),
  );
  const presets = view.presets.ok ? view.presets.value : [];
  const canCreate = presets.length && !sectionUnsupported(view.tours);
  return card('Preset tours', list, canCreate ? tourCreator(presets, context) : null);
}

function tourRow(tour, context) {
  const token = tour.token;
  const route = tour.spots.map((spot) => `${spot.preset} (${spot.stay}s)`).join(' → ');
  const controls = ['start', 'pause', 'stop'].map((operation) =>
    actionButton(context, operation, 'tour', { operation, token }, `Tour ${operation}`),
  );
  return h(
    'tr',
    {},
    h(
      'td',
      {},
      h('strong', { text: tour.name || token }),
      ' ',
      pill(tour.state, tour.state === 'touring' ? 'good' : ''),
      h('div', {
        class: 'muted small',
        text: route + (tour.auto_start ? ' · auto start' : ''),
      }),
    ),
    h(
      'td',
      {},
      h(
        'div',
        { class: 'row' },
        controls,
        actionButton(
          context,
          'remove',
          'tour',
          { operation: 'remove', token },
          'Tour removed',
          'danger',
        ),
      ),
    ),
  );
}

/** A folded form: a name, a stay time, and the presets to visit in order. */
function tourCreator(presets, context) {
  const name = h('input', { placeholder: 'Tour name', value: 'Patrol', 'aria-label': 'Tour name' });
  const stay = h('input', { type: 'number', min: 0, value: 10, 'aria-label': 'Stay seconds' });
  const picks = presets.map((preset) => [
    preset,
    h('input', { type: 'checkbox', 'aria-label': 'Include ' + preset.name }),
  ]);
  const create = async () => {
    const spots = picks
      .filter(([, box]) => box.checked)
      .map(([preset]) => ({ preset: preset.token, stay: Number(stay.value) }));
    const reply = await ptzPost(context, 'tour', { operation: 'create', name: name.value, spots });
    context.after(reply, 'Tour created');
  };
  return h(
    'details',
    {},
    h('summary', { text: 'Create a tour' }),
    row(name, h('label', {}, 'Stay (s)', stay)),
    picks.map(([preset, box]) => h('label', { class: 'check' }, box, preset.name || preset.token)),
    h(
      'div',
      { class: 'row' },
      h('button', { type: 'button', text: 'Create tour', onclick: create }),
    ),
    h('p', {
      class: 'muted small',
      text: 'A started tour keeps running on the camera until it is stopped.',
    }),
  );
}

// ---- Orientation ------------------------------------------------------------

function orientationCard(view, context) {
  const config = view.configuration.ok ? view.configuration.value : null;
  // A select of the modes the camera offers; disabled when it offers none.
  const choice = (name, modes, current) =>
    h(
      'select',
      { 'aria-label': name, disabled: !modes.length },
      (modes.length ? modes : [current || 'n/a']).map((mode) =>
        h('option', { value: mode, text: mode, selected: mode === current }),
      ),
    );
  const eflip = config ? choice('EFlip', config.eflip_modes, config.eflip) : null;
  const reverse = config ? choice('Reverse', config.reverse_modes, config.reverse) : null;
  const result = h('div', {});

  // Read the setting back: some cameras acknowledge a change and keep their own.
  const apply = h('button', {
    type: 'button',
    text: 'Apply',
    onclick: async () => {
      const body = { eflip: eflip.value, reverse: reverse.value };
      const reply = await ptzPost(context, 'direction', body, { quiet: true });
      if (!reply.ok) return result.replaceChildren(problem(reply.error));
      const path = `${base()}/ptz?profile=${encodeURIComponent(context.profile)}`;
      const check = await api('GET', path, undefined, { quiet: true });
      const stored =
        check.ok && check.data.configuration.ok ? check.data.configuration.value : null;
      const kept = stored && (stored.eflip !== body.eflip || stored.reverse !== body.reverse);
      result.replaceChildren(
        kept
          ? note(
              `The camera acknowledged the change but kept EFlip ${stored.eflip}, Reverse ${stored.reverse}.`,
            )
          : goodNote('Applied.'),
      );
    },
  });
  if (config && !config.eflip_modes.length && !config.reverse_modes.length) {
    setUnavailable('The camera lists no EFlip or Reverse modes to choose from.', apply);
  }

  return card(
    'Orientation',
    section(view.configuration, (value) =>
      h(
        'div',
        {},
        facts([
          ['Configuration', value.token],
          ['Node', value.node],
          ['Default timeout', value.default_timeout + ' s'],
        ]),
        h(
          'div',
          { class: 'row' },
          h('label', {}, 'EFlip', eflip),
          h('label', {}, 'Reverse', reverse),
          apply,
        ),
        result,
      ),
    ),
  );
}

// ---- Capabilities -----------------------------------------------------------

/** Which movement kinds the profile supports, with dimmed pills for the missing ones. */
function capabilitiesCard(view) {
  return card(
    'Capabilities',
    section(view.spaces, (spaces) => {
      const kinds = (suffix) =>
        h(
          'span',
          { class: 'row' },
          ['continuous', 'relative', 'absolute'].map((kind) =>
            spaces[kind + suffix]
              ? pill(kind, 'good')
              : h('span', {
                  class: 'pill unavailable',
                  title: 'Not supported by this profile',
                  text: kind,
                }),
          ),
        );
      const range = (value) => (value ? `${value.min} to ${value.max}` : null);
      const panTilt = spaces.continuous || spaces.relative || spaces.absolute;
      const zoom = spaces.continuous_zoom || spaces.relative_zoom || spaces.absolute_zoom;
      return facts([
        ['Pan/tilt', kinds('')],
        ['Zoom', kinds('_zoom')],
        ['Pan range', panTilt ? range(spaces.pan) : null],
        ['Tilt range', panTilt ? range(spaces.tilt) : null],
        ['Zoom range', zoom ? range(spaces.zoom) : null],
      ]);
    }),
  );
}
