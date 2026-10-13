/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// Bindings to Samsung's Tizen WASM Player (tizentvwasm.ElementaryMediaStreamSource).
//
// The source and the HTMLMediaElement it feeds live on the browser main thread, so
// everything that touches them is proxied there. Packets are appended straight from
// the calling pthread through tizentvwasm.SideThreadElementaryMediaTrack.
//
// Player state is published to C++ through a block of shared wasm memory (see
// TizenEmssState in TizenEMSS.h) instead of callbacks: Kodi's threads only poll it.
mergeInto(LibraryManager.library, {
  $TizenEmss__deps: ['$UTF8ToString', '$TizenDrm'],
  $TizenEmss: {
    // Field offsets of TizenEmssState, in bytes.
    OFF_SOURCE_STATE: 0,
    OFF_VIDEO_TRACK: 4,
    OFF_AUDIO_TRACK: 8,
    OFF_VIDEO_OPEN: 12,
    OFF_AUDIO_OPEN: 16,
    OFF_SESSION_ID: 20,
    OFF_ERROR: 24,
    OFF_ENDED: 28,
    OFF_CURRENT_TIME: 32,

    // Values of TizenEmssState::sourceState.
    STATE_DETACHED: 0,
    STATE_CLOSED: 1,
    STATE_OPEN_PENDING: 2,
    STATE_OPEN: 3,
    STATE_ENDED: 4,

    statePtr: 0,
    // Handles of the tizen_drm.js sessions the next source decrypts with.
    drmVideo: 0,
    drmAudio: 0,
    source: null,
    video: null,
    url: null,
    wantPlay: false,
    configured: false,
    rafId: 0,
    listeners: [],

    setKeys: function(track, handle) {
      var session = handle ? TizenDrm.session(handle) : null;
      if (!session) return;
      // Firmware that does not take a session per track decrypts with the keys of
      // the media element.
      try {
        track.setMediaKeySession(session);
      } catch (e) {
        console.log('[kodi][emss] setMediaKeySession: ' + e.name);
      }
    },

    // Result of tizen_emss_append() for an exception of appendPacketSync().
    appendError: function(e) {
      switch (e && e.name) {
        case 'AppendBufferFullError':
          return 1;
        case 'AppendInvalidTrackStateError':
        case 'InvalidStateError':
          return 2;
        case 'AppendIgnoredError':
        case 'AppendExpectsKeyframeError':
        case 'AppendInvalidDtsError':
        case 'AppendInvalidPtsError':
        case 'AppendNoDurationError':
          return 3;
        default:
          console.error('[kodi][emss] appendPacket: ' + (e && e.name) + ': ' + (e && e.message));
          return 4;
      }
    },

    hasPlayer: function() {
      return typeof tizentvwasm !== 'undefined' &&
             !!tizentvwasm.ElementaryMediaStreamSource &&
             !!tizentvwasm.SideThreadElementaryMediaTrack;
    },

    setI32: function(off, value) {
      if (TizenEmss.statePtr)
        Atomics.store(HEAP32, (TizenEmss.statePtr + off) >> 2, value);
    },

    setTime: function(seconds) {
      if (TizenEmss.statePtr)
        HEAPF64[(TizenEmss.statePtr + TizenEmss.OFF_CURRENT_TIME) >> 3] = seconds;
    },

    fail: function(what, error) {
      console.error('[kodi][emss] ' + what + ': ' + (error && error.message ? error.message : error));
      TizenEmss.setI32(TizenEmss.OFF_ERROR, 1);
    },

    listen: function(target, name, handler) {
      target.addEventListener(name, handler);
      TizenEmss.listeners.push([target, name, handler]);
    },

    videoElement: function() {
      var video = document.getElementById('kodi-video');
      if (!video) {
        video = document.createElement('video');
        video.id = 'kodi-video';
        var canvas = document.getElementById('canvas');
        canvas.parentNode.insertBefore(video, canvas);
      }
      // The video plane sits under the (transparent) GUI canvas.
      video.style.position = 'fixed';
      video.style.zIndex = '0';
      video.style.background = '#000';
      video.style.left = '0px';
      video.style.top = '0px';
      video.style.width = '100vw';
      video.style.height = '100vh';
      var canvasEl = document.getElementById('canvas');
      canvasEl.style.position = 'fixed';
      canvasEl.style.left = '0px';
      canvasEl.style.top = '0px';
      canvasEl.style.zIndex = '1';
      canvasEl.style.background = 'transparent';
      return video;
    },

    createSource: function() {
      try {
        return new tizentvwasm.ElementaryMediaStreamSource(
            'latency-mode-normal', 'rendering-mode-media-element');
      } catch (e) {
        // 2020 firmware only knows the single "mode" argument.
        return new tizentvwasm.ElementaryMediaStreamSource('normal');
      }
    },

    readState: function() {
      switch (TizenEmss.source ? TizenEmss.source.readyState : 'detached') {
        case 'closed': return TizenEmss.STATE_CLOSED;
        case 'open-pending': return TizenEmss.STATE_OPEN_PENDING;
        case 'open': return TizenEmss.STATE_OPEN;
        case 'ended': return TizenEmss.STATE_ENDED;
        default: return TizenEmss.STATE_DETACHED;
      }
    },

    publishState: function() {
      TizenEmss.setI32(TizenEmss.OFF_SOURCE_STATE, TizenEmss.readState());
    },

    watchTrack: function(track, openOff) {
      TizenEmss.listen(track, 'trackopen', function() {
        if (typeof track.sessionId === 'number')
          TizenEmss.setI32(TizenEmss.OFF_SESSION_ID, track.sessionId);
        TizenEmss.setI32(openOff, 1);
        TizenEmss.publishState();
      });
      TizenEmss.listen(track, 'trackclosed', function() {
        TizenEmss.setI32(openOff, 0);
        TizenEmss.publishState();
      });
      TizenEmss.listen(track, 'sessionidchanged', function(event) {
        TizenEmss.setI32(TizenEmss.OFF_SESSION_ID, event.sessionId);
      });
      TizenEmss.listen(track, 'appenderror', function(event) {
        console.warn('[kodi][emss] append error on track ' + track.trackId, event);
      });
    },

    tick: function() {
      if (!TizenEmss.video) return;
      TizenEmss.setTime(TizenEmss.video.currentTime);
      TizenEmss.rafId = requestAnimationFrame(TizenEmss.tick);
    },

    tryPlay: function() {
      var video = TizenEmss.video;
      if (!video || !TizenEmss.wantPlay || !video.paused) return;
      var promise = video.play();
      if (promise && promise.catch)
        promise.catch(function(e) { console.warn('[kodi][emss] play(): ' + e.message); });
    },

    close: function() {
      if (TizenEmss.rafId) cancelAnimationFrame(TizenEmss.rafId);
      TizenEmss.rafId = 0;
      TizenEmss.listeners.forEach(function(l) { l[0].removeEventListener(l[1], l[2]); });
      TizenEmss.listeners = [];
      var video = TizenEmss.video;
      if (video) {
        try { video.pause(); } catch (e) {}
        video.removeAttribute('src');
        try { video.load(); } catch (e) {}
        video.style.visibility = 'hidden';
      }
      if (TizenEmss.source && typeof TizenEmss.source.destroy === 'function') {
        try { TizenEmss.source.destroy(); } catch (e) {}
      }
      if (TizenEmss.url) URL.revokeObjectURL(TizenEmss.url);
      TizenEmss.setI32(TizenEmss.OFF_VIDEO_OPEN, 0);
      TizenEmss.setI32(TizenEmss.OFF_AUDIO_OPEN, 0);
      TizenEmss.setI32(TizenEmss.OFF_SOURCE_STATE, TizenEmss.STATE_DETACHED);
      TizenEmss.source = null;
      TizenEmss.video = null;
      TizenEmss.url = null;
      TizenEmss.statePtr = 0;
      TizenEmss.wantPlay = false;
      TizenEmss.configured = false;
    },
  },

  tizen_emss_available__deps: ['$TizenEmss'],
  tizen_emss_available__proxy: 'sync',
  tizen_emss_available__sig: 'i',
  tizen_emss_available: function() {
    return TizenEmss.hasPlayer() ? 1 : 0;
  },

  // Returns 1 when the platform accepts the config, 0 when it rejects it and -1
  // when this firmware cannot tell in advance.
  tizen_emss_validate_video__deps: ['$TizenEmss'],
  tizen_emss_validate_video__proxy: 'sync',
  tizen_emss_validate_video__sig: 'ipiiii',
  tizen_emss_validate_video: function(mime, width, height, fpsNum, fpsDen) {
    if (!TizenEmss.hasPlayer()) return 0;
    var Track = tizentvwasm.ElementaryMediaTrack;
    if (!Track || typeof Track.ValidateVideoConfig !== 'function') return -1;
    try {
      var result = Track.ValidateVideoConfig({
        mimeType: UTF8ToString(mime),
        extradata: new Uint8Array(0),
        width: width,
        height: height,
        framerateNum: fpsNum,
        framerateDen: fpsDen,
      }, 'latency-mode-normal', 'rendering-mode-media-element');
      if (result !== 'ConfigSupported')
        console.log('[kodi][emss] video config rejected: ' + result);
      return result === 'ConfigSupported' ? 1 : 0;
    } catch (e) {
      return -1;
    }
  },

  tizen_emss_validate_audio__deps: ['$TizenEmss'],
  tizen_emss_validate_audio__proxy: 'sync',
  tizen_emss_validate_audio__sig: 'ipppi',
  tizen_emss_validate_audio: function(mime, sampleFormat, channelLayout, sampleRate) {
    if (!TizenEmss.hasPlayer()) return 0;
    var Track = tizentvwasm.ElementaryMediaTrack;
    if (!Track || typeof Track.ValidateAudioConfig !== 'function') return -1;
    try {
      var result = Track.ValidateAudioConfig({
        mimeType: UTF8ToString(mime),
        extradata: new Uint8Array(0),
        sampleFormat: UTF8ToString(sampleFormat),
        channelLayout: UTF8ToString(channelLayout),
        samplesPerSecond: sampleRate,
      });
      if (result !== 'ConfigSupported')
        console.log('[kodi][emss] audio config rejected: ' + result);
      return result === 'ConfigSupported' ? 1 : 0;
    } catch (e) {
      return -1;
    }
  },

  // Creates the source, attaches it to the <video> element, adds the tracks and
  // opens it. Completion is reported through the state block: sourceState becomes
  // STATE_OPEN and the track "open" flags are raised, or error is set.
  // cfg points at a TizenEmssConfig (see TizenEMSS.h).
  tizen_emss_open__deps: ['$TizenEmss'],
  tizen_emss_open__proxy: 'sync',
  tizen_emss_open__sig: 'ipp',
  tizen_emss_open: function(statePtr, cfg) {
    if (!TizenEmss.hasPlayer()) return 0;
    if (TizenEmss.source) TizenEmss.close();

    var bytes = function(ptr, size) {
      // The platform rejects views on shared memory for extradata; copy it out.
      return size > 0 ? new Uint8Array(HEAPU8.slice(ptr, ptr + size)) : new Uint8Array(0);
    };

    var duration = HEAPF64[cfg >> 3];
    var videoConfig = {
      mimeType: UTF8ToString(HEAP32[(cfg + 8) >> 2]),
      extradata: bytes(HEAP32[(cfg + 12) >> 2], HEAP32[(cfg + 16) >> 2]),
      width: HEAP32[(cfg + 20) >> 2],
      height: HEAP32[(cfg + 24) >> 2],
      framerateNum: HEAP32[(cfg + 28) >> 2],
      framerateDen: HEAP32[(cfg + 32) >> 2],
    };
    var hasAudio = HEAP32[(cfg + 36) >> 2] !== 0;
    var audioConfig = hasAudio ? {
      mimeType: UTF8ToString(HEAP32[(cfg + 40) >> 2]),
      extradata: bytes(HEAP32[(cfg + 44) >> 2], HEAP32[(cfg + 48) >> 2]),
      sampleFormat: UTF8ToString(HEAP32[(cfg + 52) >> 2]),
      channelLayout: UTF8ToString(HEAP32[(cfg + 56) >> 2]),
      samplesPerSecond: HEAP32[(cfg + 60) >> 2],
    } : null;

    TizenEmss.statePtr = statePtr;
    TizenEmss.setI32(TizenEmss.OFF_ERROR, 0);
    TizenEmss.setI32(TizenEmss.OFF_ENDED, 0);
    TizenEmss.setI32(TizenEmss.OFF_VIDEO_OPEN, 0);
    TizenEmss.setI32(TizenEmss.OFF_AUDIO_OPEN, 0);
    TizenEmss.setI32(TizenEmss.OFF_SESSION_ID, 0);
    TizenEmss.setTime(0);

    var source;
    var video;
    try {
      source = TizenEmss.createSource();
      video = TizenEmss.videoElement();
    } catch (e) {
      TizenEmss.fail('creating ElementaryMediaStreamSource', e);
      TizenEmss.statePtr = 0;
      return 0;
    }
    TizenEmss.source = source;
    TizenEmss.video = video;
    TizenEmss.configured = false;
    TizenEmss.wantPlay = false;

    var configure = function() {
      if (TizenEmss.configured || source.readyState !== 'closed') return;
      TizenEmss.configured = true;
      try {
        source.duration = duration;
        var videoTrack = source.addVideoTrack(videoConfig);
        TizenEmss.setKeys(videoTrack, TizenEmss.drmVideo);
        TizenEmss.watchTrack(videoTrack, TizenEmss.OFF_VIDEO_OPEN);
        TizenEmss.setI32(TizenEmss.OFF_VIDEO_TRACK, videoTrack.trackId);
        if (audioConfig) {
          var audioTrack = source.addAudioTrack(audioConfig);
          TizenEmss.setKeys(audioTrack, TizenEmss.drmAudio);
          TizenEmss.watchTrack(audioTrack, TizenEmss.OFF_AUDIO_OPEN);
          TizenEmss.setI32(TizenEmss.OFF_AUDIO_TRACK, audioTrack.trackId);
        }
        var opening = source.open();
        if (opening && opening.catch)
          opening.catch(function(e) { TizenEmss.fail('opening the source', e); });
      } catch (e) {
        TizenEmss.fail('configuring tracks', e);
      }
    };

    ['sourceopen', 'sourceclosed', 'sourcedetached', 'sourceended'].forEach(function(name) {
      TizenEmss.listen(source, name, function() {
        TizenEmss.publishState();
        if (name === 'sourceended') TizenEmss.setI32(TizenEmss.OFF_ENDED, 1);
        configure();
      });
    });
    TizenEmss.listen(source, 'playbackpositionchanged', function(event) {
      TizenEmss.setTime(event.playbackPosition);
    });
    TizenEmss.listen(source, 'error', function(event) {
      TizenEmss.fail('pipeline error', event && event.message ? event : 'unknown');
    });
    TizenEmss.listen(video, 'error', function() {
      TizenEmss.fail('media element error', video.error ? video.error.message : 'unknown');
    });
    TizenEmss.listen(video, 'canplay', TizenEmss.tryPlay);
    TizenEmss.listen(video, 'ended', function() { TizenEmss.setI32(TizenEmss.OFF_ENDED, 1); });

    var mediaKeys = TizenDrm.keys(TizenEmss.drmVideo) || TizenDrm.keys(TizenEmss.drmAudio);
    if (mediaKeys && video.mediaKeys !== mediaKeys) {
      video.setMediaKeys(mediaKeys).catch(function(e) {
        TizenEmss.fail('setting the media keys', e);
      });
    }

    video.style.visibility = 'visible';
    TizenEmss.url = URL.createObjectURL(source);
    video.src = TizenEmss.url;
    TizenEmss.publishState();
    // Attaching may already have moved the source to "closed".
    configure();
    TizenEmss.rafId = requestAnimationFrame(TizenEmss.tick);
    return 1;
  },

  tizen_emss_close__deps: ['$TizenEmss'],
  tizen_emss_close__proxy: 'sync',
  tizen_emss_close__sig: 'v',
  tizen_emss_close: function() {
    TizenEmss.close();
  },

  tizen_emss_play__deps: ['$TizenEmss'],
  tizen_emss_play__proxy: 'async',
  tizen_emss_play__sig: 'v',
  tizen_emss_play: function() {
    TizenEmss.wantPlay = true;
    TizenEmss.tryPlay();
  },

  tizen_emss_pause__deps: ['$TizenEmss'],
  tizen_emss_pause__proxy: 'async',
  tizen_emss_pause__sig: 'v',
  tizen_emss_pause: function() {
    TizenEmss.wantPlay = false;
    if (TizenEmss.video && !TizenEmss.video.paused) TizenEmss.video.pause();
  },

  tizen_emss_set_rate__deps: ['$TizenEmss'],
  tizen_emss_set_rate__proxy: 'async',
  tizen_emss_set_rate__sig: 'vd',
  tizen_emss_set_rate: function(rate) {
    if (!TizenEmss.video) return;
    try { TizenEmss.video.playbackRate = rate; } catch (e) {
      console.warn('[kodi][emss] playbackRate ' + rate + ': ' + e.message);
    }
  },

  // Seeking the media element closes the tracks and reopens them under a new
  // session id; the caller waits for that through the state block.
  tizen_emss_seek__deps: ['$TizenEmss'],
  tizen_emss_seek__proxy: 'sync',
  tizen_emss_seek__sig: 'id',
  tizen_emss_seek: function(seconds) {
    if (!TizenEmss.video) return 0;
    try {
      TizenEmss.setI32(TizenEmss.OFF_VIDEO_OPEN, 0);
      TizenEmss.setI32(TizenEmss.OFF_AUDIO_OPEN, 0);
      TizenEmss.setI32(TizenEmss.OFF_ENDED, 0);
      TizenEmss.video.currentTime = seconds;
      TizenEmss.setTime(seconds);
      return 1;
    } catch (e) {
      console.warn('[kodi][emss] seek to ' + seconds + ': ' + e.message);
      return 0;
    }
  },

  // Destination rectangle of the video plane, in CSS pixels.
  tizen_emss_set_rect__deps: ['$TizenEmss'],
  tizen_emss_set_rect__proxy: 'async',
  tizen_emss_set_rect__sig: 'viiii',
  tizen_emss_set_rect: function(x, y, width, height) {
    var video = TizenEmss.video;
    if (!video) return;
    video.style.left = x + 'px';
    video.style.top = y + 'px';
    video.style.width = width + 'px';
    video.style.height = height + 'px';
  },

  // Not proxied: runs on the calling pthread.
  // Returns 0 appended, 1 buffer full (retry), 2 track not accepting data (retry
  // after it reopens), 3 packet dropped by the platform, 4 fatal.
  tizen_emss_append__deps: ['$TizenEmss'],
  tizen_emss_append__sig: 'iipidddii',
  tizen_emss_append: function(trackId, data, size, pts, dts, duration, keyFrame, sessionId) {
    try {
      tizentvwasm.SideThreadElementaryMediaTrack.appendPacketSync(trackId, {
        pts: pts,
        dts: dts,
        duration: duration,
        isKeyFrame: keyFrame !== 0,
        isEncrypted: false,
        sessionId: sessionId,
      }, HEAPU8.subarray(data, data + size));
      return 0;
    } catch (e) {
      return TizenEmss.appendError(e);
    }
  },

  // tizen_emss_append() for a packet the TV has to decrypt. keyId and iv are 16
  // bytes each; subsamples holds subsampleCount pairs of clear and encrypted byte
  // counts as 32 bit integers.
  tizen_emss_append_encrypted__deps: ['$TizenEmss'],
  tizen_emss_append_encrypted__sig: 'iipidddiipppii',
  tizen_emss_append_encrypted: function(trackId, data, size, pts, dts, duration, keyFrame,
                                        sessionId, keyId, iv, subsamples, subsampleCount, cbcs) {
    var subs = [];
    for (var i = 0; i < subsampleCount; i++) {
      subs.push({
        clearBytes: HEAPU32[(subsamples >> 2) + 2 * i],
        encryptedBytes: HEAPU32[(subsamples >> 2) + 2 * i + 1],
      });
    }
    try {
      tizentvwasm.SideThreadElementaryMediaTrack.appendPacketSync(trackId, {
        pts: pts,
        dts: dts,
        duration: duration,
        isKeyFrame: keyFrame !== 0,
        isEncrypted: true,
        sessionId: sessionId,
        keyId: HEAPU8.slice(keyId, keyId + 16),
        initializationVector: HEAPU8.slice(iv, iv + 16),
        encryptionMode: cbcs ? 'cbcs' : 'cenc',
        subsamples: subs,
      }, HEAPU8.subarray(data, data + size));
      return 0;
    } catch (e) {
      return TizenEmss.appendError(e);
    }
  },

  // Sessions of tizen_drm.js whose keys the tracks of the next tizen_emss_open()
  // decrypt with; 0 for a track that is not encrypted.
  tizen_emss_set_media_keys__deps: ['$TizenEmss'],
  tizen_emss_set_media_keys__proxy: 'sync',
  tizen_emss_set_media_keys__sig: 'vii',
  tizen_emss_set_media_keys: function(videoSession, audioSession) {
    TizenEmss.drmVideo = videoSession;
    TizenEmss.drmAudio = audioSession;
  },

  tizen_emss_end_of_track__sig: 'vii',
  tizen_emss_end_of_track: function(trackId, sessionId) {
    try {
      tizentvwasm.SideThreadElementaryMediaTrack.appendEndOfTrackSync(trackId, sessionId);
    } catch (e) {
      console.warn('[kodi][emss] appendEndOfTrack: ' + (e && e.message));
    }
  },
});
