// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Team Kodi
//
// Main-thread setup for Kodi's WASM build: profile persistence, canvas focus
// and clipboard paste. Rendering goes through a WebGL context Emscripten
// creates on <canvas id="canvas"> and proxies to the Kodi pthread.

// Persist Kodi's user profile ($HOME/.kodi) to IndexedDB via IDBFS so that
// sources.xml, guisettings.xml, the databases, etc. survive a page refresh.
// Mount and populate run inside Module.preRun, and addRunDependency blocks
// main() until IDBFS has finished loading from IndexedDB.
(function installIdbfsPersistence() {
  var Module = globalThis.Module = globalThis.Module || {};
  var PROFILE_PATH = '/home/web_user/.kodi';

  Module.preRun = Module.preRun || [];
  Module.preRun.push(function mountKodiProfile() {
    try {
      FS.mkdirTree(PROFILE_PATH);
    } catch (e) {
      console.warn('[kodi] mkdirTree ' + PROFILE_PATH + ':', e);
    }

    // A file that cannot grow because the browser is out of memory must fail the write
    // like a full disk would. An exception other than an FS error is not caught by the
    // system call, and then the thread that waits for its result never wakes up.
    var expandFileStorage = MEMFS.expandFileStorage;
    MEMFS.expandFileStorage = function (node, newCapacity) {
      try {
        return expandFileStorage.call(MEMFS, node, newCapacity);
      } catch (e) {
        if (e instanceof RangeError) {
          console.warn('[kodi] out of memory growing a file to ' + newCapacity + ' bytes');
          throw new FS.ErrnoError(51); // ENOSPC
        }
        throw e;
      }
    };

    // Downloads, unpacked add-on packages and cached thumbnails are not kept: storing a
    // package that is still being downloaded over and over can use up the memory of a
    // TV, and its storage is small (80 MB on a 2025 Samsung), too small for the artwork
    // of a library or an EPG. Left out of the profile on disk, they are deleted from it
    // with the next flush and not loaded again. The texture database goes with the
    // thumbnails, as it would otherwise point to files that are gone.
    var TRANSIENT = ['/temp', '/addons/packages', '/addons/temp', '/userdata/Thumbnails']
        .map(function (dir) { return PROFILE_PATH + dir; });
    var TEXTURE_DB = PROFILE_PATH + '/userdata/Database/Textures';
    function isTransient(path) {
      return path.startsWith(TEXTURE_DB) || TRANSIENT.some(function (dir) {
        return path === dir || path.startsWith(dir + '/');
      });
    }
    function withoutTransient(getSet, filter) {
      return function (mount, callback) {
        return getSet.call(IDBFS, mount, function (err, set) {
          if (!err && filter()) {
            Object.keys(set.entries).filter(isTransient).forEach(function (path) {
              delete set.entries[path];
            });
          }
          callback(err, set);
        });
      };
    }
    var populating = false;
    IDBFS.getLocalSet = withoutTransient(IDBFS.getLocalSet, function () { return true; });
    IDBFS.getRemoteSet = withoutTransient(IDBFS.getRemoteSet, function () { return populating; });

    // What the storage holds, for Kodi to tell whether an add-on still fits.
    function updateStorageEstimate() {
      if (typeof navigator === 'undefined' || !navigator.storage || !navigator.storage.estimate) {
        return;
      }
      navigator.storage.estimate().then(function (estimate) {
        globalThis.kodiStorage = { quota: estimate.quota, usage: estimate.usage };
      }, function () {});
    }

    try {
      FS.mount(IDBFS, {}, PROFILE_PATH);
    } catch (e) {
      console.warn('[kodi] IDBFS mount failed, profile will not persist:', e);
      return;
    }

    // Storing the profile first goes through all of its files, which blocks the main
    // thread for 180 ms with 2000 files on a TV, long enough to make the video stutter.
    // So the profile is only stored after a change to a file that is kept.
    var dirty = false;
    function markDirty(path) {
      if (typeof path === 'string' && path.startsWith(PROFILE_PATH + '/') && !isTransient(path)) {
        dirty = true;
      }
    }
    function trackChanges(name, pathOf) {
      var original = FS[name];
      FS[name] = function () {
        try {
          markDirty(pathOf.apply(null, arguments));
        } catch (e) {
        }
        return original.apply(FS, arguments);
      };
    }
    function firstArg(path) { return path; }
    function streamPath(stream) { return stream && stream.path; }
    ['mknod', 'unlink', 'rmdir', 'truncate', 'chmod', 'utime'].forEach(function (name) {
      trackChanges(name, firstArg);
    });
    ['write', 'ftruncate', 'fchmod'].forEach(function (name) {
      trackChanges(name, function (stream) {
        return typeof stream === 'number' ? streamPath(FS.getStream(stream)) : streamPath(stream);
      });
    });
    trackChanges('symlink', function (target, path) { return path; });
    trackChanges('rename', function (oldPath, newPath) {
      markDirty(oldPath);
      return newPath;
    });

    // Debounced background flush to IndexedDB.
    var syncing = false;
    var pending = false;
    function flushToIdb() {
      if (syncing) { pending = true; return; }
      syncing = true;
      FS.syncfs(false, function (err) {
        syncing = false;
        if (err) { console.warn('[kodi] IDBFS persist:', err.name || err, err.message || ''); }
        updateStorageEstimate();
        if (pending) { pending = false; flushToIdb(); }
      });
    }

    // Block main() until the profile has been loaded from IndexedDB. A flush
    // before then would delete whatever has not been loaded yet.
    addRunDependency('kodi-idbfs-populate');
    populating = true;
    FS.syncfs(true, function (err) {
      populating = false;
      updateStorageEstimate();
      removeRunDependency('kodi-idbfs-populate');
      if (err) {
        console.warn('[kodi] IDBFS populate failed, profile will not persist:', err);
        return;
      }

      setInterval(function () {
        if (dirty) {
          dirty = false;
          flushToIdb();
        }
      }, 5000);

      if (typeof window !== 'undefined') {
        window.addEventListener('pagehide', flushToIdb, { capture: true });
        window.addEventListener('beforeunload', flushToIdb, { capture: true });
        // A TV can stop a hidden app without either of the above firing.
        document.addEventListener('visibilitychange', function () {
          if (document.visibilityState === 'hidden') { flushToIdb(); }
        });
      }
    });
  });
})();

(function () {
  if (typeof document === 'undefined') {
    return; // pthread worker
  }

  var Module = globalThis.Module = globalThis.Module || {};
  var kodi = (Module.kodi = Module.kodi || {});

  var canvas = document.getElementById('canvas');
  if (!canvas) {
    console.warn('[kodi] No <canvas id="canvas"> found; rendering disabled.');
    return;
  }
  // tabindex=0 allows the canvas to receive focus so paste and keyboard reach Kodi.
  if (canvas.getAttribute('tabindex') === '-1' || canvas.getAttribute('tabindex') === null) {
    canvas.setAttribute('tabindex', '0');
  }

  // The WebGL context is created on this canvas by Emscripten (proxied from the
  // Kodi pthread); nothing else may take a rendering context on it.
  kodi.canvas = canvas;

  var prevOnRuntime = Module.onRuntimeInitialized;
  Module.onRuntimeInitialized = function () {
    // Must run on the browser main thread (document is undefined on pthread workers).
    document.addEventListener(
        'paste',
        function (e) {
          // A paste into a DOM field (the native keyboard's input) is the browser's.
          var t = e.target;
          if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) {
            return;
          }
          try {
            var text = (e.clipboardData && e.clipboardData.getData)
                ? e.clipboardData.getData('text/plain')
                : String();
            if (text === undefined || text === null) {
              text = String();
            }
            e.preventDefault();
            if (typeof Module.ccall === 'function') {
              Module.ccall('kodi_wasm_dispatch_paste', null, ['string'], [text]);
            }
          } catch (err) {
            console.error('[kodi] paste handler:', err);
          }
        },
        true);
    try { canvas.focus(); } catch (_) {}
    if (typeof prevOnRuntime === 'function') {
      try { prevOnRuntime(); } catch (e) { console.error(e); }
    }
  };
})();
