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

    usable: function(session) {
      var usable = 0;
      session.keyStatuses.forEach(function(status) {
        if (status === 'usable') usable++;
      });
      return usable;
    },

    // An open session of the key system for which the key is usable.
    holder: function(system, keyId) {
      for (var handle in TizenDrm.sessions) {
        var entry = TizenDrm.sessions[handle];
        if (entry.closed || !entry.session || entry.system !== system) continue;
        var found = false;
        entry.session.keyStatuses.forEach(function(status, id) {
          var bytes = new Uint8Array(id.buffer ? id.buffer.slice(id.byteOffset, id.byteOffset + id.byteLength) : id);
          if (status === 'usable' && bytes.length === 16 && bytes.every(function(b, i) { return b === keyId[i]; }))
            found = true;
        });
        if (found) return entry;
      }
      return null;
    },

    // The MediaKeys the session of a handle belongs to.
    keys: function(handle) {
      var entry = TizenDrm.sessions[handle];
      return entry ? entry.mediaKeys : null;
    },
  },

  tizen_drm_open__deps: ['$TizenDrm'],
  tizen_drm_open__proxy: 'sync',
  tizen_drm_open__sig: 'ippppip',
  tizen_drm_open: function(statePtr, keySystem, initDataType, initData, initDataSize, keyId) {
    if (!navigator.requestMediaKeySystemAccess) return 0;

    var handle = TizenDrm.nextHandle++;
    var type = UTF8ToString(initDataType);
    var system = UTF8ToString(keySystem);

    var key = keyId ? HEAPU8.slice(keyId, keyId + 16) : null;
    var owner = key ? TizenDrm.holder(system, key) : null;
    if (owner) {
      owner.shared.refs++;
      TizenDrm.sessions[handle] = {
        statePtr: statePtr, system: system, keyId: key, session: owner.session,
        mediaKeys: owner.mediaKeys, shared: owner.shared, messages: [], closed: false,
      };
      Atomics.store(HEAP32, (statePtr + TizenDrm.OFF_USABLE_KEYS) >> 2, TizenDrm.usable(owner.session));
      return handle;
    }

    var entry = {
      statePtr: statePtr, system: system, keyId: key, session: null, mediaKeys: null,
      shared: { refs: 1 }, messages: [], closed: false,
    };
    TizenDrm.sessions[handle] = entry;

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
        TizenDrm.store(entry, TizenDrm.OFF_USABLE_KEYS, TizenDrm.usable(session));
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

  tizen_drm_usable_key_id__deps: ['$TizenDrm'],
  tizen_drm_usable_key_id__proxy: 'sync',
  tizen_drm_usable_key_id__sig: 'iip',
  tizen_drm_usable_key_id: function(handle, keyId) {
    var entry = TizenDrm.sessions[handle];
    if (!entry || !entry.session) return 0;
    // Preferably the key the session was opened for, the one of the stream.
    var found = null;
    entry.session.keyStatuses.forEach(function(status, id) {
      var bytes = new Uint8Array(id.buffer ? id.buffer.slice(id.byteOffset, id.byteOffset + id.byteLength) : id);
      if (status !== 'usable' || bytes.length !== 16) return;
      var own = entry.keyId && bytes.every(function(b, i) { return b === entry.keyId[i]; });
      if (own || !found) found = bytes;
    });
    if (!found) return 0;
    HEAPU8.set(found, keyId);
    return 1;
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
    if (--entry.shared.refs > 0) return;
    if (entry.session) entry.session.close().catch(function() {});
  },
});
