/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// HTTP transport behind CurlWasm.cpp. Requests run as fetch() on the browser main
// thread, which is the one with an event loop; the response is handed to the wasm
// side piece by piece, and reading pauses while Kodi has enough of it queued.
mergeInto(LibraryManager.library, {
  $KodiHttp__deps: ['malloc', '$stringToNewUTF8', 'kodi_wasm_http_on_headers',
                    'kodi_wasm_http_on_data', 'kodi_wasm_http_on_done'],
  $KodiHttp: {
    ERROR_NETWORK: 1,
    ERROR_TIMEOUT: 2,

    requests: {},

    finish: function(id, req, error) {
      if (KodiHttp.requests[id] !== req) return;
      delete KodiHttp.requests[id];
      clearTimeout(req.timer);
      _kodi_wasm_http_on_done(id, error);
    },

    pump: function(id, req) {
      if (req.pumping || req.paused || !req.reader) return;
      req.pumping = true;
      var next = function() {
        req.reader.read().then(function(result) {
          if (KodiHttp.requests[id] !== req) return;
          if (result.done) {
            KodiHttp.finish(id, req, 0);
            return;
          }
          var chunk = result.value;
          var ptr = _malloc(chunk.length);
          if (!ptr) {
            req.controller.abort();
            KodiHttp.finish(id, req, KodiHttp.ERROR_NETWORK);
            return;
          }
          HEAPU8.set(chunk, ptr);
          if (_kodi_wasm_http_on_data(id, ptr, chunk.length)) {
            req.paused = true;
            req.pumping = false;
            return;
          }
          next();
        }, function() {
          KodiHttp.finish(id, req, req.timedOut ? KodiHttp.ERROR_TIMEOUT : KodiHttp.ERROR_NETWORK);
        });
      };
      next();
    },
  },

  // headers: "Name: value" lines separated by "\n".
  kodi_wasm_http_start__deps: ['$KodiHttp', '$UTF8ToString'],
  kodi_wasm_http_start__proxy: 'sync',
  kodi_wasm_http_start__sig: 'vippppii',
  kodi_wasm_http_start: function(id, method, url, headers, body, bodySize, timeoutMs) {
    var req = {
      controller: new AbortController(),
      reader: null,
      pumping: false,
      paused: false,
      timedOut: false,
      timer: 0,
    };
    KodiHttp.requests[id] = req;

    var init = { method: UTF8ToString(method), signal: req.controller.signal, headers: {} };
    UTF8ToString(headers).split('\n').forEach(function(line) {
      var colon = line.indexOf(':');
      if (colon > 0) init.headers[line.substring(0, colon).trim()] = line.substring(colon + 1).trim();
    });
    // fetch() refuses views on shared memory, so the body is copied out.
    if (bodySize > 0) init.body = HEAPU8.slice(body, body + bodySize);

    if (timeoutMs > 0) {
      req.timer = setTimeout(function() {
        req.timedOut = true;
        req.controller.abort();
      }, timeoutMs);
    }

    fetch(UTF8ToString(url), init).then(function(response) {
      clearTimeout(req.timer);
      if (KodiHttp.requests[id] !== req) return;

      var lines = [];
      response.headers.forEach(function(value, name) { lines.push(name + ': ' + value); });
      var keep = _kodi_wasm_http_on_headers(id, response.status, stringToNewUTF8(response.url),
                                            stringToNewUTF8(lines.join('\n')));
      if (!keep || !response.body) {
        if (response.body) req.controller.abort();
        KodiHttp.finish(id, req, 0);
        return;
      }
      req.reader = response.body.getReader();
      KodiHttp.pump(id, req);
    }, function() {
      KodiHttp.finish(id, req, req.timedOut ? KodiHttp.ERROR_TIMEOUT : KodiHttp.ERROR_NETWORK);
    });
  },

  kodi_wasm_http_resume__deps: ['$KodiHttp'],
  kodi_wasm_http_resume__proxy: 'async',
  kodi_wasm_http_resume__sig: 'vi',
  kodi_wasm_http_resume: function(id) {
    var req = KodiHttp.requests[id];
    if (!req) return;
    req.paused = false;
    KodiHttp.pump(id, req);
  },

  kodi_wasm_http_abort__deps: ['$KodiHttp'],
  kodi_wasm_http_abort__proxy: 'async',
  kodi_wasm_http_abort__sig: 'vi',
  kodi_wasm_http_abort: function(id) {
    var req = KodiHttp.requests[id];
    if (!req) return;
    delete KodiHttp.requests[id];
    clearTimeout(req.timer);
    req.controller.abort();
  },
});
