/**
 * @file tabs/raw.js
 * @brief The Raw request tab: send any ONVIF operation and see the answer.
 *
 * For operations wlonvif has no typed call for. The server wraps the body in a
 * SOAP envelope, signs it with the camera's credentials, and sends it to the
 * endpoint the camera advertises for the chosen service.
 */

import { api } from '../api.js';
import { copy, h, indentXml, note, pill, row, spinner, wideCard } from '../dom.js';
import { errorBox } from '../errors.js';
import { base, camera } from '../state.js';

/** [service, SOAP body] pairs offered in the Examples list. */
const rawExamples = [
  ['device', '<tds:GetDeviceInformation/>'],
  ['device', '<tds:GetZeroConfiguration/>'],
  ['device', '<tds:GetRelayOutputs/>'],
  ['media', '<trt:GetAudioOutputs/>'],
  ['media', '<trt:GetVideoEncoderConfigurations/>'],
  ['ptz', '<tptz:GetNodes/>'],
  ['events', '<tev:GetEventProperties/>'],
  [
    'imaging',
    '<timg:GetMoveOptions><timg:VideoSourceToken>SOURCE</timg:VideoSourceToken></timg:GetMoveOptions>',
  ],
];

const services = ['device', 'media', 'ptz', 'events', 'analytics', 'imaging'];

export async function renderRaw(fill) {
  const connected = camera().connected;
  const service = h(
    'select',
    { 'aria-label': 'Service' },
    services.map((name) => h('option', { value: name, text: name })),
  );
  const body = h('textarea', { rows: 6, 'aria-label': 'SOAP body', spellcheck: 'false' });
  body.value = rawExamples[0][1];
  const response = h('input', {
    placeholder: 'Response element (default: operation + Response)',
    'aria-label': 'Response element',
  });

  // "device: GetDeviceInformation" and so on.
  const examples = h(
    'select',
    { 'aria-label': 'Examples' },
    h('option', { value: '', text: 'Examples…' }),
    rawExamples.map(([name, xml], index) =>
      h('option', { value: index, text: `${name}: ${xml.match(/<[a-z]+:(\w+)/)[1]}` }),
    ),
  );
  examples.onchange = () => {
    const [name, xml] = rawExamples[Number(examples.value)] || [];
    if (xml) {
      service.value = name;
      body.value = xml;
    }
  };

  const output = h('div', {});
  const send = h('button', {
    class: 'primary',
    type: 'button',
    text: 'Send',
    disabled: !connected,
  });
  send.onclick = async () => {
    send.disabled = true;
    output.replaceChildren(spinner('Waiting for the camera…'));
    const request = { service: service.value, body: body.value, response: response.value };
    const reply = await api('POST', base() + '/call', request, { quiet: true });
    send.disabled = false;
    output.replaceChildren(reply.ok ? answer(reply.data) : errorBox(reply.error));
  };

  fill(
    wideCard(
      'Raw ONVIF request',
      h(
        'p',
        { class: 'muted' },
        "Send any operation, signed with the camera's credentials, to the endpoint the camera " +
          'advertises for the service. Declared prefixes: tds trt tptz tev tan timg tt wsnt tns1.',
      ),
      connected ? null : note('Sign in to send requests.'),
      row(h('label', {}, 'Service', service), examples),
      body,
      response,
      row(send),
      output,
    ),
  );
}

/** The camera's response element, indented, with a copy button. */
function answer(xml) {
  const copyButton = h('button', {
    class: 'small',
    type: 'button',
    text: 'Copy',
    onclick: () => copy(xml),
  });
  return h(
    'div',
    {},
    row(pill('answered', 'good'), copyButton),
    h('pre', { text: indentXml(xml) || '(empty response element)' }),
  );
}
