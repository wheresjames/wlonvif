/**
 * @file api.js
 * @brief Calls to the explorer server's JSON API.
 */

import { toast } from './dom.js';
import { errorSummary } from './errors.js';

/**
 * Call the explorer API and return its envelope: `{ ok: true, data }` or
 * `{ ok: false, error }`. Never throws: an unreachable server or a non-JSON
 * answer becomes an error envelope too, so callers handle one shape.
 *
 * @param method   HTTP method.
 * @param path     Such as `/api/cameras/camera-1/ptz`.
 * @param body     Sent as JSON when given.
 * @param options  `{ quiet: true }` skips the error toast, for callers that show
 *                 the error themselves or poll in the background.
 */
export async function api(method, path, body, options = {}) {
  const failure = (category, message) => ({
    ok: false,
    error: { category, operation: 'Explorer API', message },
  });
  let reply;
  try {
    const response = await fetch(path, {
      method,
      headers: body !== undefined ? { 'Content-Type': 'application/json' } : {},
      body: body !== undefined ? JSON.stringify(body) : undefined,
      credentials: 'same-origin', // The session cookie.
    });
    try {
      reply = await response.json();
    } catch {
      reply = failure('protocol-violation', 'Unexpected HTTP ' + response.status);
    }
  } catch (unreachable) {
    reply = failure('network', 'The explorer server is unreachable: ' + unreachable.message);
  }
  if (!reply.ok && !options.quiet) toast(errorSummary(reply.error), 'bad');
  return reply;
}
