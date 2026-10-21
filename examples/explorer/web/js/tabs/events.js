/**
 * @file tabs/events.js
 * @brief The Events tab: topic list and a live event monitor.
 *
 * The server keeps the event subscription and the newest events; this tab
 * polls every second for events newer than the last one it has shown. The
 * monitor keeps running when the tab is left, so events are not missed.
 */

import { api } from '../api.js';
import { card, h, pill, row, setUnavailable, wideCard } from '../dom.js';
import { errorBox, isUnsupported, problem } from '../errors.js';
import { base, tabTimer } from '../state.js';

/** The most rows kept on the page; older ones drop off the bottom. */
const shownEvents = 300;

export async function renderEvents(fill, current) {
  const reply = await api('POST', base() + '/topics', {});
  if (!current()) return;
  const topics = reply.ok ? reply.data : [];
  const boxes = topics.map((topic) => [
    topic,
    h('input', { type: 'checkbox', 'aria-label': topic.topic }),
  ]);

  const start = h('button', { class: 'primary', type: 'button', text: 'Start monitoring' });
  const stop = h('button', { type: 'button', text: 'Stop' });
  const clear = h('button', { type: 'button', text: 'Clear events' });
  if (!reply.ok && isUnsupported(reply.error)) {
    setUnavailable('The camera does not support ONVIF events.', start, stop);
  }
  const state = h('div', {});
  const rows = h('tbody', {});

  let since = 0; // Sequence number of the newest event shown.
  const poll = async () => {
    const view = await api('GET', `${base()}/events?since=${since}`, undefined, { quiet: true });
    if (!current() || !view.ok) return;
    state.replaceChildren(
      monitorState(view.data),
      view.data.error ? errorBox(view.data.error, 'The event stream failed') : '',
    );
    for (const event of view.data.events) {
      since = Math.max(since, event.sequence);
      rows.prepend(eventRow(event)); // Newest first.
      while (rows.childElementCount > shownEvents) rows.lastChild.remove();
    }
  };

  start.onclick = async () => {
    // No ticked topics means every topic.
    const selected = boxes.filter(([, box]) => box.checked).map(([topic]) => topic.topic);
    const result = await api('POST', base() + '/events/start', { topics: selected });
    if (result.ok) poll();
  };
  stop.onclick = async () => {
    await api('POST', base() + '/events/stop', {});
    poll();
  };
  clear.onclick = async () => {
    const result = await api('POST', base() + '/events/clear', {});
    if (result.ok) rows.replaceChildren();
  };

  const eventsTable = h(
    'table',
    {},
    h(
      'thead',
      {},
      h(
        'tr',
        {},
        ['Time', 'Topic', 'Source', 'Data'].map((name) => h('th', { text: name })),
      ),
    ),
    rows,
  );
  fill(
    h(
      'div',
      { class: 'grid' },
      card('Topics', reply.ok ? topicList(boxes) : problem(reply.error), row(start, stop), state),
      wideCard(
        'Events',
        row(
          h('span', {
            class: 'muted small',
            text: 'Trigger a detector (move in front of the camera, or pan it) to see events arrive.',
          }),
          clear,
        ),
        eventsTable,
      ),
    ),
  );
  await poll();
  tabTimer(poll, 1000);
}

/** The camera's topics as checkboxes, with their data fields. */
function topicList(boxes) {
  if (!boxes.length) return h('p', { class: 'muted', text: 'The camera lists no topics.' });
  const item = ([topic, box]) =>
    h(
      'label',
      { class: 'check' },
      box,
      h(
        'span',
        {},
        h('span', { class: 'mono small', text: topic.topic.replace('tns1:', '') }),
        // A "state" topic reports a condition that stays true or false, such as motion.
        topic.property ? [' ', pill('state')] : null,
        topic.data.length
          ? h('div', { class: 'muted small', text: 'data: ' + topic.data.join(', ') })
          : null,
      ),
    );
  return h(
    'div',
    {},
    h('p', {
      class: 'muted small',
      text: 'Tick topics to have the camera send only those; leave all unticked for everything.',
    }),
    boxes.map(item),
  );
}

function monitorState(data) {
  if (!data.running) return h('span', { class: 'muted', text: 'Not monitoring' });
  const scope = data.topics.length ? ` to ${data.topics.length} topic(s)` : ' to every topic';
  return h('span', { class: 'good' }, h('span', { class: 'spinner' }), ' Listening' + scope);
}

/** An event, or a note that the subscription had to be renewed and events may be lost. */
function eventRow(event) {
  const cell = (text) => h('td', { class: 'small mono', text });
  return h(
    'tr',
    {},
    cell(event.time),
    event.health
      ? h('td', { class: 'warn', colspan: 3, text: event.health })
      : [cell(event.topic.replace('tns1:', '')), cell(event.source), cell(event.data)],
  );
}
