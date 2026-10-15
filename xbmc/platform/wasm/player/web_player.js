/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// Browser side of CWebPlayer (WebPlayer.h): plays a DASH or HLS manifest with
// Shaka Player in a <video> element under the GUI canvas, with DRM through the
// browser's Encrypted Media Extensions.
//
// Everything runs on the browser main thread, so every entry point is proxied there.
mergeInto(LibraryManager.library, {
  $WebPlayer__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  $WebPlayer: {
    // Shaka Player is shipped next to index.html and only loaded when needed.
    SHAKA_URL: 'shaka-player.compiled.js',

    // Values of the state block (WebPlayerState in WebPlayer.h), as doubles.
    TIME: 0,
    DURATION: 1,
    PAUSED: 2,
    ENDED: 3,
    ERROR: 4,
    BUFFERING: 5,
    WIDTH: 6,
    HEIGHT: 7,
    LOADED: 8,
    COUNT: 9,

    shaka: null,
    player: null,
    video: null,
    loaded: false,
    ended: false,
    error: 0,

    loadShaka: function() {
      if (!WebPlayer.shaka) {
        WebPlayer.shaka = new Promise(function(resolve, reject) {
          if (typeof shaka !== 'undefined') return resolve();
          var script = document.createElement('script');
          script.src = WebPlayer.SHAKA_URL;
          script.onload = function() { resolve(); };
          script.onerror = function() { reject(new Error('cannot load ' + WebPlayer.SHAKA_URL)); };
          document.head.appendChild(script);
        }).then(function() { shaka.polyfill.installAll(); });
        WebPlayer.shaka.catch(function() { WebPlayer.shaka = null; });
      }
      return WebPlayer.shaka;
    },

    videoElement: function() {
      var video = document.getElementById('kodi-web-video');
      if (!video) {
        video = document.createElement('video');
        video.id = 'kodi-web-video';
        var canvas = document.getElementById('canvas');
        canvas.parentNode.insertBefore(video, canvas);
      }
      // The video sits under the GUI canvas, which is transparent where no GUI is drawn.
      video.style.position = 'fixed';
      video.style.zIndex = '0';
      video.style.background = '#000';
      video.style.left = '0px';
      video.style.top = '0px';
      video.style.width = '100vw';
      video.style.height = '100vh';
      video.style.visibility = 'visible';
      var canvasEl = document.getElementById('canvas');
      canvasEl.style.position = 'fixed';
      canvasEl.style.zIndex = '1';
      canvasEl.style.background = 'transparent';
      return video;
    },

    // "a=b&c=d", with URL encoded values, as InputStream Adaptive takes headers.
    parseHeaders: function(text) {
      var headers = {};
      if (!text) return headers;
      text.split('&').forEach(function(pair) {
        var i = pair.indexOf('=');
        if (i <= 0) return;
        try {
          headers[decodeURIComponent(pair.slice(0, i))] = decodeURIComponent(pair.slice(i + 1));
        } catch (e) {
          headers[pair.slice(0, i)] = pair.slice(i + 1);
        }
      });
      return headers;
    },

    // Kodi appends request headers to a URL after a "|".
    splitUrl: function(url) {
      var i = url.indexOf('|');
      return i < 0 ? { url: url, headers: {} }
                   : { url: url.slice(0, i), headers: WebPlayer.parseHeaders(url.slice(i + 1)) };
    },

    base64ToBytes: function(text) {
      var bin = atob(text.replace(/-/g, '+').replace(/_/g, '/'));
      var bytes = new Uint8Array(bin.length);
      for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      return bytes;
    },

    // The DRM configuration of InputStream Adaptive's properties, in the forms add-ons use:
    // drm_legacy "<key system>|<license url>|<headers>", license_type with license_key
    // "<license url>|<headers>|...", and the JSON of drm.
    drmConfig: function(props) {
      var config = { servers: {}, advanced: {}, clearKeys: {} };
      var licenseHeaders = {};
      var add = function(keySystem, url, headers, certificate) {
        if (!keySystem) return;
        if (keySystem === 'org.w3.clearkey' && url && url.indexOf('://') < 0) {
          // Key ids and keys: "kid:key,kid:key".
          url.split(',').forEach(function(pair) {
            var parts = pair.split(':');
            if (parts.length === 2) config.clearKeys[parts[0].trim()] = parts[1].trim();
          });
          return;
        }
        if (url) config.servers[keySystem] = url;
        if (headers) licenseHeaders[keySystem] = WebPlayer.parseHeaders(headers);
        if (certificate) {
          config.advanced[keySystem] = { serverCertificate: WebPlayer.base64ToBytes(certificate) };
        }
      };

      var legacy = props['inputstream.adaptive.drm_legacy'];
      if (legacy) {
        var parts = legacy.split('|');
        add(parts[0], parts[1], parts[2]);
      }

      var type = props['inputstream.adaptive.license_type'];
      var key = props['inputstream.adaptive.license_key'];
      if (type && key) {
        var keyParts = key.split('|');
        add(type, keyParts[0], keyParts[1]);
      }
      var certificate = props['inputstream.adaptive.server_certificate'];
      if (type && certificate) {
        config.advanced[type] = { serverCertificate: WebPlayer.base64ToBytes(certificate) };
      }

      var drm = props['inputstream.adaptive.drm'];
      if (drm) {
        try {
          var systems = JSON.parse(drm);
          Object.keys(systems).forEach(function(keySystem) {
            var license = systems[keySystem].license || {};
            var headers = license.req_headers;
            if (headers && typeof headers === 'object') {
              headers = Object.keys(headers).map(function(name) {
                return encodeURIComponent(name) + '=' + encodeURIComponent(headers[name]);
              }).join('&');
            }
            add(keySystem, license.server_url || (license.keys ? Object.keys(license.keys).map(
                function(kid) { return kid + ':' + license.keys[kid]; }).join(',') : ''),
                headers, license.server_certificate);
          });
        } catch (e) {
          console.warn('[kodi][web] inputstream.adaptive.drm: ' + e.message);
        }
      }

      if (!Object.keys(config.clearKeys).length) delete config.clearKeys;
      return { config: config, licenseHeaders: licenseHeaders };
    },

    close: function() {
      var player = WebPlayer.player;
      WebPlayer.player = null;
      WebPlayer.loaded = false;
      WebPlayer.ended = false;
      WebPlayer.error = 0;
      if (player) player.destroy().catch(function() {});
      var video = WebPlayer.video;
      WebPlayer.video = null;
      if (video) {
        video.removeAttribute('src');
        try { video.load(); } catch (e) {}
        video.style.visibility = 'hidden';
      }
    },
  },

  // Opens url, with the item properties as JSON, from startSeconds on. Returns 0 if
  // there is no browser support for it at all; failures later on are reported by the state.
  web_player_open__deps: ['$WebPlayer'],
  web_player_open__proxy: 'sync',
  web_player_open__sig: 'ippd',
  web_player_open: function(urlPtr, propsPtr, startSeconds) {
    if (typeof MediaSource === 'undefined') return 0;
    WebPlayer.close();

    var target = WebPlayer.splitUrl(UTF8ToString(urlPtr));
    var props = {};
    try { props = JSON.parse(UTF8ToString(propsPtr)); } catch (e) {}
    var drm = WebPlayer.drmConfig(props);
    var streamHeaders = Object.assign(
        {}, target.headers,
        WebPlayer.parseHeaders(props['inputstream.adaptive.stream_headers']));
    var manifestHeaders = Object.assign(
        {}, streamHeaders,
        WebPlayer.parseHeaders(props['inputstream.adaptive.manifest_headers']));

    var video = WebPlayer.videoElement();
    WebPlayer.video = video;
    WebPlayer.loaded = false;
    WebPlayer.ended = false;
    WebPlayer.error = 0;

    video.onended = function() { WebPlayer.ended = true; };

    WebPlayer.loadShaka().then(function() {
      if (WebPlayer.video !== video) return;
      var player = new shaka.Player();
      WebPlayer.player = player;
      return player.attach(video).then(function() {
        // Shaka starts low and slowly works its way up; a TV is usually on a fast
        // connection, and a stream that is too much for it is stepped down quickly.
        player.configure({ drm: drm.config, abr: { defaultBandwidthEstimate: 50000000 } });
        var types = shaka.net.NetworkingEngine.RequestType;
        player.getNetworkingEngine().registerRequestFilter(function(type, request) {
          var headers = type === types.LICENSE ? drm.licenseHeaders[player.keySystem()] || {}
                      : type === types.MANIFEST ? manifestHeaders
                      : type === types.SEGMENT ? streamHeaders : {};
          Object.keys(headers).forEach(function(name) { request.headers[name] = headers[name]; });
        });
        player.addEventListener('error', function(event) {
          var error = event.detail || {};
          console.error('[kodi][web] shaka error ' + error.code + ': ' + JSON.stringify(error.data || []).slice(0, 300));
          WebPlayer.error = error.code || 1;
        });
        console.log('[kodi][web] loading, DRM: ' + JSON.stringify(Object.keys(drm.config.servers)) +
                    (drm.config.clearKeys ? ' and clear keys' : ''));
        return player.load(target.url, startSeconds > 0 ? startSeconds : null);
      }).then(function() {
        if (WebPlayer.player !== player) return;
        WebPlayer.loaded = true;
        var promise = video.play();
        if (promise && promise.catch) promise.catch(function(e) { console.warn('[kodi][web] play(): ' + e.message); });
      });
    }).catch(function(e) {
      console.error('[kodi][web] ' + (e && e.code ? 'shaka error ' + e.code : (e && e.message) || e));
      WebPlayer.error = (e && e.code) || 1;
    });
    return 1;
  },

  web_player_close__deps: ['$WebPlayer'],
  web_player_close__proxy: 'sync',
  web_player_close__sig: 'v',
  web_player_close: function() {
    WebPlayer.close();
  },

  // Fills the WebPlayerState doubles at statePtr.
  web_player_poll__deps: ['$WebPlayer'],
  web_player_poll__proxy: 'sync',
  web_player_poll__sig: 'vp',
  web_player_poll: function(statePtr) {
    var base = statePtr >> 3;
    var video = WebPlayer.video;
    var player = WebPlayer.player;
    var time = 0;
    var duration = 0;
    if (video) {
      time = video.currentTime;
      duration = isFinite(video.duration) ? video.duration : 0;
      if (player && player.isLive && player.isLive()) {
        // A live stream is shown as the window that can be seeked in.
        var range = player.seekRange();
        time -= range.start;
        duration = range.end - range.start;
      }
    }
    HEAPF64[base + WebPlayer.TIME] = time * 1000;
    HEAPF64[base + WebPlayer.DURATION] = duration * 1000;
    HEAPF64[base + WebPlayer.PAUSED] = video && video.paused ? 1 : 0;
    HEAPF64[base + WebPlayer.ENDED] = WebPlayer.ended ? 1 : 0;
    HEAPF64[base + WebPlayer.ERROR] = WebPlayer.error;
    HEAPF64[base + WebPlayer.BUFFERING] = player && player.isBuffering() ? 1 : 0;
    // The size of the active track; the TV reports another one for protected video.
    var width = video ? video.videoWidth : 0;
    var height = video ? video.videoHeight : 0;
    if (player && WebPlayer.loaded) {
      var active = player.getVariantTracks().filter(function(t) { return t.active; })[0];
      if (active && active.width && active.height) {
        width = active.width;
        height = active.height;
      }
    }
    HEAPF64[base + WebPlayer.WIDTH] = width;
    HEAPF64[base + WebPlayer.HEIGHT] = height;
    HEAPF64[base + WebPlayer.LOADED] = WebPlayer.loaded ? 1 : 0;
  },

  web_player_pause__deps: ['$WebPlayer'],
  web_player_pause__proxy: 'sync',
  web_player_pause__sig: 'vi',
  web_player_pause: function(pause) {
    var video = WebPlayer.video;
    if (!video) return;
    if (pause) {
      video.pause();
    } else {
      var promise = video.play();
      if (promise && promise.catch) promise.catch(function() {});
    }
  },

  // Seeks to a time in milliseconds, in the terms of web_player_poll().
  web_player_seek__deps: ['$WebPlayer'],
  web_player_seek__proxy: 'sync',
  web_player_seek__sig: 'vd',
  web_player_seek: function(ms) {
    var video = WebPlayer.video;
    var player = WebPlayer.player;
    if (!video) return;
    var seconds = ms / 1000;
    if (player && player.isLive && player.isLive()) seconds += player.seekRange().start;
    video.currentTime = Math.max(0, seconds);
  },

  // Returns the audio languages and text tracks as JSON, to be freed by the caller.
  web_player_tracks__deps: ['$WebPlayer'],
  web_player_tracks__proxy: 'sync',
  web_player_tracks__sig: 'p',
  web_player_tracks: function() {
    var player = WebPlayer.player;
    var result = { audio: [], audioIndex: -1, text: [], textIndex: -1, textVisible: false };
    if (player && WebPlayer.loaded) {
      var variants = player.getVariantTracks();
      var seen = {};
      variants.forEach(function(track) {
        var key = track.language + '|' + (track.roles || []).join(',') + '|' + (track.channelsCount || 0);
        if (!(key in seen)) {
          seen[key] = result.audio.length;
          result.audio.push({ language: track.language || '', name: track.label || '',
                              roles: track.roles || [], channels: track.channelsCount || 0,
                              codec: track.audioCodec || '' });
        }
        if (track.active) result.audioIndex = seen[key];
      });
      player.getTextTracks().forEach(function(track, i) {
        result.text.push({ language: track.language || '', name: track.label || '', id: track.id,
                           forced: !!track.forced });
        if (track.active) result.textIndex = i;
      });
      result.textVisible = player.isTextTrackVisible();
    }
    return stringToNewUTF8(JSON.stringify(result));
  },

  web_player_set_audio__deps: ['$WebPlayer'],
  web_player_set_audio__proxy: 'sync',
  web_player_set_audio__sig: 'vpi',
  web_player_set_audio: function(languagePtr, channels) {
    var player = WebPlayer.player;
    if (!player) return;
    var language = UTF8ToString(languagePtr);
    var track = player.getVariantTracks().filter(function(t) {
      return t.language === language && (!channels || t.channelsCount === channels);
    })[0];
    if (track) player.selectAudioLanguage(track.language, (track.roles || [])[0], channels || undefined);
  },

  web_player_set_text__deps: ['$WebPlayer'],
  web_player_set_text__proxy: 'sync',
  web_player_set_text__sig: 'vii',
  web_player_set_text: function(index, visible) {
    var player = WebPlayer.player;
    if (!player) return;
    var tracks = player.getTextTracks();
    if (index >= 0 && index < tracks.length) player.selectTextTrack(tracks[index]);
    player.setTextTrackVisibility(visible !== 0);
  },
});
