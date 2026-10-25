/*
 * cwist-wasm HTTP framing: the serialized HTTP/1.1 request and response that
 * cross the cwist-guest dispatch boundary (wit/cwist.wit), built from and
 * parsed into the fetch-shaped objects the adapter exposes.
 */
'use strict';

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder();

/**
 * Build a raw HTTP/1.1 request from a fetch-like init object.
 * @param {{method?: string, path?: string, headers?: Object, body?: (string|Uint8Array)}} init
 * @returns {Uint8Array}
 */
function buildRequestBytes(init) {
  const method = (init.method || 'GET').toUpperCase();
  const path = init.path || '/';
  const headers = init.headers || {};
  let body = init.body;
  if (typeof body === 'string') body = ENCODER.encode(body);
  if (body == null) body = new Uint8Array(0);

  let head = method + ' ' + path + ' HTTP/1.1\r\nHost: wasm\r\n';
  let hasLength = false;
  for (const [k, v] of Object.entries(headers)) {
    if (k.toLowerCase() === 'content-length') hasLength = true;
    head += k + ': ' + v + '\r\n';
  }
  if (body.length > 0 && !hasLength) head += 'Content-Length: ' + body.length + '\r\n';
  head += 'Connection: close\r\n\r\n';

  const headBytes = ENCODER.encode(head);
  const out = new Uint8Array(headBytes.length + body.length);
  out.set(headBytes, 0);
  out.set(body, headBytes.length);
  return out;
}

/**
 * Parse a serialized HTTP/1.1 response into parts.
 * @param {Uint8Array} bytes
 * @returns {{status: number, statusText: string, headers: Object, body: Uint8Array}}
 */
function parseResponseBytes(bytes) {
  // Header/body split: find CRLFCRLF. Responses from dispatch_memory always
  // carry the full header block before the body.
  let sep = -1;
  for (let i = 0; i + 3 < bytes.length; i++) {
    if (bytes[i] === 13 && bytes[i + 1] === 10 && bytes[i + 2] === 13 && bytes[i + 3] === 10) {
      sep = i;
      break;
    }
  }
  if (sep < 0) throw new Error('cwist-wasm: malformed response (no header terminator)');
  const headText = DECODER.decode(bytes.subarray(0, sep));
  const lines = headText.split('\r\n');
  const statusMatch = /^HTTP\/\d\.\d (\d{3}) ?(.*)$/.exec(lines[0] || '');
  if (!statusMatch) throw new Error('cwist-wasm: malformed status line: ' + JSON.stringify(lines[0]));
  const headers = {};
  for (let i = 1; i < lines.length; i++) {
    const idx = lines[i].indexOf(':');
    if (idx > 0) headers[lines[i].slice(0, idx).trim()] = lines[i].slice(idx + 1).trim();
  }
  // Copy: the caller may reuse the response buffer for the next dispatch.
  const body = bytes.slice(sep + 4);
  return {
    status: parseInt(statusMatch[1], 10),
    statusText: statusMatch[2],
    headers,
    body,
  };
}

module.exports = { buildRequestBytes, parseResponseBytes };
