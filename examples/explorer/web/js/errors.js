/**
 * @file errors.js
 * @brief Showing errors: what the camera answered, and when it simply lacks a feature.
 */

import { copy, facts, h, missing } from './dom.js';

/** One line, such as "GetSnapshotUri failed: device returned a SOAP Fault (ter:ActionNotSupported)". */
export function errorSummary(error) {
  if (!error) return 'Unknown error';
  const code = error.soap
    ? error.soap.subcode || error.soap.code
    : error.http_status
      ? 'HTTP ' + error.http_status
      : '';
  return `${error.operation || 'Request'} failed: ${error.message || error.category}${code ? ' (' + code + ')' : ''}`;
}

/**
 * Everything an error carries, laid out for debugging: the library's category
 * and operation, HTTP status, and the camera's SOAP Fault codes, with a hint
 * and a button that copies it all for a bug report.
 */
export function errorBox(error, title) {
  error = error || {};
  const soap = error.soap || {};
  const box = h(
    'div',
    { class: 'error' },
    h('div', { class: 'error-title', text: title || errorSummary(error) }),
    facts([
      ['Category', error.category],
      ['Operation', error.operation],
      ['Message', error.message],
      ['HTTP status', error.http_status],
      ['SOAP code', soap.code],
      ['SOAP subcode', soap.subcode],
      ['SOAP reason', soap.reason],
      ['SOAP detail', soap.detail],
      ['Retryable', error.retryable ? 'yes' : null],
      [
        'May have applied',
        error.may_have_applied ? 'yes; the device may have acted before failing' : null,
      ],
    ]),
    hint(error),
    error.attempts
      ? h(
          'details',
          {},
          h('summary', { text: 'Attempts' }),
          h('pre', { text: JSON.stringify(error.attempts, null, 2) }),
        )
      : null,
    h(
      'div',
      { class: 'row' },
      h('button', {
        class: 'small',
        type: 'button',
        text: 'Copy details',
        onclick: () => copy(JSON.stringify(error, null, 2)),
      }),
    ),
  );
  return box;
}

/** Plain-language guidance for common failure categories. */
function hint(error) {
  const code = (error.soap && (error.soap.subcode || error.soap.code)) || '';
  const tips = {
    authentication:
      'The camera rejected the credentials. Check the username and password; some cameras need a separate ONVIF account.',
    authorization: 'The account is valid but not allowed to do this; use an administrator account.',
    timeout: 'The camera did not answer in time. It may be busy, rebooting, or unreachable.',
    network: 'The camera could not be reached. Check that it is on and the address is right.',
    unsupported: 'The camera does not offer this feature.',
    'endpoint-rejected':
      'The camera advertised a service on another host, which wlonvif refuses for safety.',
  };
  let text = tips[error.category];
  if (code.includes('ActionNotSupported'))
    text = 'The camera does not implement this ONVIF operation.';
  if (code.includes('NotAuthorized')) text = tips.authentication;
  if (code.includes('ScopeOverwrite') || code.includes('FixedScope'))
    text = 'Fixed scopes are built into the camera and cannot be changed.';
  return text ? h('div', { class: 'muted small', text }) : null;
}

/** Whether an error says the camera lacks a feature, rather than that something broke. */
export function isUnsupported(error) {
  if (!error) return false;
  const code = (error.soap && (error.soap.subcode || error.soap.code)) || '';
  return error.category === 'unsupported' || /NotSupported/.test(code);
}
/** Whether a view section failed only because the camera lacks the feature. */
export const sectionUnsupported = (value) =>
  Boolean(value && !value.ok && isUnsupported(value.error));

/** An error box, or a quiet note with the details folded away when the camera simply lacks the feature. */
export function problem(error, title) {
  if (!isUnsupported(error)) return errorBox(error, title);
  return h(
    'details',
    { class: 'missing-details' },
    h(
      'summary',
      {},
      missing(`Not supported by this camera${error.operation ? ' (' + error.operation + ')' : ''}`),
    ),
    errorBox(error),
  );
}

/**
 * Render a view section: `{ ok: true, value }` through @p render, or its error.
 * Views hold one section per camera query, so one failure does not hide the rest.
 */
export function section(value, render) {
  if (!value) return null;
  return value.ok ? render(value.value) : problem(value.error);
}
