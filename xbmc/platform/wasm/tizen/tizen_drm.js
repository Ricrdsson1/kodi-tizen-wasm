/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// DRM sessions of the browser's Encrypted Media Extensions (see TizenDRM.h).
//
// Sessions live on the browser main thread, so every entry point is proxied
// there. Their state is published through a block of shared wasm memory
// (TizenDrmState), which the calling thread polls.
mergeInto(LibraryManager.library, {
  $TizenDrm__deps: ['$UTF8ToString'],
  $TizenDrm: {
    // Field offsets of TizenDrmState, in bytes.
    OFF_MESSAGES: 0,
    OFF_UPDATES: 4,
    OFF_USABLE_KEYS: 8,
    OFF_ERROR: 12,

    nextHandle: 1,
    sessions: {},
    // One MediaKeys per key system, as a media element takes a single one.
    mediaKeys: {},

    add: function(entry, off) {
      if (entry.statePtr) Atomics.add(HEAP32, (entry.statePtr + off) >> 2, 1);
    },

    store: function(entry, off, value) {
      if (entry.statePtr) Atomics.store(HEAP32, (entry.statePtr + off) >> 2, value);
    },

    fail: function(entry, what, error) {
      console.error('[kodi][drm] ' + what + ': ' + (error && error.message ? error.message : error));
      TizenDrm.store(entry, TizenDrm.OFF_ERROR, 1);
    },

    // The session a track of the Tizen WASM Player decrypts with.
    session: function(handle) {
      var entry = TizenDrm.sessions[handle];
      return entry ? entry.session : null;
    },

    // The MediaKeys the session of a handle belongs to.
    keys: function(handle) {
      var entry = TizenDrm.sessions[handle];
      return entry ? entry.mediaKeys : null;
    },
  },

  tizen_drm_open__deps: ['$TizenDrm'],
  tizen_drm_open__proxy: 'sync',
  tizen_drm_open__sig: 'ippppi',
  tizen_drm_open: function(statePtr, keySystem, initDataType, initData, initDataSize) {
    if (!navigator.requestMediaKeySystemAccess) return 0;

    var handle = TizenDrm.nextHandle++;
    var entry = { statePtr: statePtr, session: null, mediaKeys: null, messages: [], closed: false };
    TizenDrm.sessions[handle] = entry;

    var type = UTF8ToString(initDataType);
    var system = UTF8ToString(keySystem);
    // Views on shared memory are refused; the init data is copied out.
    var data = HEAPU8.slice(initData, initData + initDataSize);
    var config = [{
      initDataTypes: [type],
      videoCapabilities: [{ contentType: 'video/mp4; codecs="avc1.640028"' }],
      audioCapabilities: [{ contentType: 'audio/mp4; codecs="mp4a.40.2"' }],
    }];

    if (!TizenDrm.mediaKeys[system]) {
      TizenDrm.mediaKeys[system] =
          navigator.requestMediaKeySystemAccess(system, config).then(function(access) {
            return access.createMediaKeys();
          });
      TizenDrm.mediaKeys[system].catch(function() { delete TizenDrm.mediaKeys[system]; });
    }

    TizenDrm.mediaKeys[system].then(function(mediaKeys) {
      if (entry.closed) return;
      var session = mediaKeys.createSession();
      entry.mediaKeys = mediaKeys;
      entry.session = session;
      session.addEventListener('message', function(event) {
        entry.messages.push(new Uint8Array(event.message));
        TizenDrm.add(entry, TizenDrm.OFF_MESSAGES);
      });
      session.addEventListener('keystatuseschange', function() {
        var usable = 0;
        session.keyStatuses.forEach(function(status) {
          if (status === 'usable') usable++;
        });
        TizenDrm.store(entry, TizenDrm.OFF_USABLE_KEYS, usable);
      });
      return session.generateRequest(type, data);
    }).catch(function(e) {
      TizenDrm.fail(entry, 'opening a ' + system + ' session', e);
    });

    return handle;
  },

  tizen_drm_read_message__deps: ['$TizenDrm'],
  tizen_drm_read_message__proxy: 'sync',
  tizen_drm_read_message__sig: 'iipi',
  tizen_drm_read_message: function(handle, buffer, capacity) {
    var entry = TizenDrm.sessions[handle];
    if (!entry || !entry.messages.length) return 0;
    var message = entry.messages[0];
    if (message.length > capacity) return -message.length;
    entry.messages.shift();
    HEAPU8.set(message, buffer);
    return message.length;
  },

  tizen_drm_update__deps: ['$TizenDrm'],
  tizen_drm_update__proxy: 'sync',
  tizen_drm_update__sig: 'vipi',
  tizen_drm_update: function(handle, response, size) {
    var entry = TizenDrm.sessions[handle];
    if (!entry || !entry.session) return;
    entry.session.update(HEAPU8.slice(response, response + size)).then(function() {
      TizenDrm.add(entry, TizenDrm.OFF_UPDATES);
    }, function(e) {
      TizenDrm.fail(entry, 'updating the session', e);
    });
  },

  tizen_drm_close__deps: ['$TizenDrm'],
  tizen_drm_close__proxy: 'sync',
  tizen_drm_close__sig: 'vi',
  tizen_drm_close: function(handle) {
    var entry = TizenDrm.sessions[handle];
    if (!entry) return;
    delete TizenDrm.sessions[handle];
    entry.closed = true;
    entry.statePtr = 0;
    if (entry.session) entry.session.close().catch(function() {});
  },
});
