/**
 * @file dom.js
 * @brief DOM building, small page widgets, and text formatting.
 *
 * h() is the only way the page creates elements: text always goes in as text,
 * never parsed as markup, so camera answers cannot inject HTML. The page's
 * Content-Security-Policy forbids inline scripts as a second line of defence.
 */

export const $ = (selector) => document.querySelector(selector);

/**
 * Build an element.
 *
 * @param tag       Element name, such as `'div'`.
 * @param props     Attributes, plus: `class`, `text` (text content), `onX` (event
 *                  listeners), and `dataset`. null, undefined, and false are skipped,
 *                  and true sets an empty attribute.
 * @param children  Nodes or text, nested in arrays at any depth; null, undefined,
 *                  and false are skipped, so conditional children can be inline.
 */
export function h(tag, props, ...children) {
  const element = document.createElement(tag);
  for (const [key, value] of Object.entries(props || {})) {
    if (value === undefined || value === null || value === false) continue;
    if (key === 'class') element.className = value;
    else if (key === 'text') element.textContent = value;
    else if (key.startsWith('on')) element.addEventListener(key.slice(2), value);
    else if (key === 'dataset') Object.assign(element.dataset, value);
    else if (value === true) element.setAttribute(key, '');
    else element.setAttribute(key, String(value));
  }
  for (const child of children.flat(Infinity)) {
    if (child === null || child === undefined || child === false) continue;
    element.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return element;
}

// ---- Layout -----------------------------------------------------------------

/** A name/value list from `[name, value]` pairs; empty values are left out. */
export const facts = (pairs) =>
  h(
    'dl',
    { class: 'facts' },
    pairs
      .filter((pair) => pair && pair[1] !== undefined && pair[1] !== null && pair[1] !== '')
      .map(([name, value]) => [
        h('dt', { text: name }),
        h('dd', {}, value instanceof Node ? value : String(value)),
      ]),
  );

/** A table with optional column headings; @p rows are `tr` elements. */
export const table = (headings, rows) =>
  h(
    'table',
    {},
    headings
      ? h(
          'thead',
          {},
          h(
            'tr',
            {},
            headings.map((heading) => h('th', { text: heading })),
          ),
        )
      : null,
    h('tbody', {}, rows),
  );

/** A horizontal group of controls that wraps on narrow screens. */
export const row = (...children) => h('div', { class: 'row' }, ...children);

export const card = (title, ...children) =>
  h(
    'section',
    { class: 'card' },
    h('div', { class: 'card-head' }, h('h2', { text: title })),
    ...children,
  );

/** A card spanning the whole grid row. */
export const wideCard = (title, ...children) => {
  const element = card(title, ...children);
  element.classList.add('wide');
  return element;
};

// ---- Small widgets ----------------------------------------------------------

/** A rounded label; @p tone is `good`, `warn`, `bad`, or empty. */
export const pill = (text, tone) => h('span', { class: 'pill ' + (tone || ''), text });
export const spinner = (text) =>
  h('div', { class: 'muted' }, h('span', { class: 'spinner' }), ' ', text || 'Loading…');
/** A highlighted remark, such as "the camera kept its own value". */
export const note = (text) => h('div', { class: 'note', text });
export const goodNote = (text) => h('div', { class: 'success-note', text });

/** A message in the corner that disappears by itself; errors stay longer. */
export function toast(text, tone) {
  const element = h('div', { class: 'toast ' + (tone || ''), text });
  $('#toasts').append(element);
  setTimeout(() => element.remove(), tone === 'bad' ? 9000 : 4000);
}

export function copy(text) {
  navigator.clipboard?.writeText(text).then(
    () => toast('Copied to the clipboard', 'good'),
    () => toast('Copy failed; select the text instead', 'bad'),
  );
}

/** Save @p text as a file called @p name. */
export function download(name, text, type) {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const link = h('a', { href: url, download: name });
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
}

// ---- Missing features -------------------------------------------------------

/** A line where a missing feature's controls would be. */
export const missing = (text) => h('div', { class: 'missing small', text });

/**
 * Dim and disable controls for a feature the camera lacks, with the reason as
 * a tooltip. A null reason restores them. Elements may be controls or
 * containers of controls, in arrays at any depth.
 */
export function setUnavailable(reason, ...elements) {
  for (const element of elements.flat(Infinity)) {
    if (!element) continue;
    element.classList.toggle('unavailable', Boolean(reason));
    if (reason) element.title = reason;
    else element.removeAttribute('title');
    const controls = element.matches('button, input, select, textarea')
      ? [element]
      : element.querySelectorAll('button, input, select, textarea');
    for (const control of controls) control.disabled = Boolean(reason);
  }
}

// ---- Text -------------------------------------------------------------------

export const formatBytes = (bytes) =>
  bytes > 1e6 ? (bytes / 1e6).toFixed(1) + ' MB' : Math.max(1, Math.round(bytes / 1e3)) + ' kB';

/** `http://www.onvif.org/ver10/media/wsdl` → `ver10/media`. */
export const shortNamespace = (uri) =>
  uri.replace('http://www.onvif.org/', '').replace('/wsdl', '');

/** Indent XML for display; an element holding only text stays on one line. */
export function indentXml(xml) {
  const tokens = xml.replace(/>\s+</g, '><').match(/<[^>]+>|[^<]+/g) || [];
  const lines = [];
  let depth = 0;
  const pad = () => '  '.repeat(depth);
  for (let i = 0; i < tokens.length; i++) {
    const token = tokens[i];
    if (token.startsWith('</')) {
      depth = Math.max(0, depth - 1);
      lines.push(pad() + token);
    } else if (token.startsWith('<')) {
      const opens = !token.endsWith('/>') && !token.startsWith('<?') && !token.startsWith('<!');
      // <a>text</a> on one line.
      const textOnly =
        opens && tokens[i + 1] && !tokens[i + 1].startsWith('<') && tokens[i + 2]?.startsWith('</');
      if (textOnly) {
        lines.push(pad() + token + tokens[i + 1].trim() + tokens[i + 2]);
        i += 2;
        continue;
      }
      lines.push(pad() + token);
      if (opens) depth++;
    } else if (token.trim()) {
      lines.push(pad() + token.trim());
    }
  }
  return lines.join('\n');
}
