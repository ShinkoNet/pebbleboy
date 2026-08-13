var CMD = {
  ROM_INFO_REQUEST: 10,
  ROM_INFO: 11,
  ROM_ERROR: 16,
  ROM_LIST_REQUEST: 30,
  ROM_LIST_BEGIN: 31,
  ROM_LIST_ITEM: 32,
  ROM_LIST_END: 33,
  ROM_SELECT: 34,
  ROM_INSTALL_DATA: 40,
  ROM_INSTALL_ACK: 41,
  ROM_INSTALL_END: 42,
  ROM_INSTALL_DONE: 43
};

var BANK_SIZE = 16 * 1024;
var MSG_CHUNK = 512;
var CACHE_CHUNK = 8192;
var CACHE_SAVE_INITIAL_DELAY_MS = 5000;
var CACHE_SAVE_IDLE_DELAY_MS = 250;
var CACHE_SAVE_CHUNK_DELAY_MS = 25;
var MAX_ROM_SIZE = 8 * 1024 * 1024;
var MAX_ROM_LIBRARY = 12;
var MAX_SEND_RETRIES = 5;
var ROM_INFO_COALESCE_MS = 5000;
var CONFIG_URL = 'https://ptv.netcavy.net/gb/?v=2';
var romBytes = null;
var romMeta = null;
var appMessageQueue = [];
var appMessageBusy = false;
var romLoading = false;
var romLoadUrl = null;
var romLoadCallbacks = [];
var romInfoPending = false;
var romInfoPendingUrl = null;
var cacheSaveSerial = 0;
var crc32Table = null;

function normalizeRomLibrary(value) {
  var out = [];
  if (!(value instanceof Array)) {
    return out;
  }
  for (var i = 0; i < value.length && out.length < MAX_ROM_LIBRARY; i++) {
    var item = value[i] || {};
    var url = String(item.url || '').trim();
    if (!url) {
      continue;
    }
    var name = String(item.name || '').trim();
    out.push({
      name: (name || ('ROM ' + (out.length + 1))).slice(0, 32),
      url: url
    });
  }
  return out;
}

function romLibrary() {
  try {
    var stored = JSON.parse(localStorage.getItem('romLibrary') || 'null');
    var library = normalizeRomLibrary(stored);
    if (library.length) {
      return library;
    }
  } catch (err) {
    console.log('pebbleboy: bad ROM library: ' + err);
  }

  // Transparently preserve configurations made by pre-library builds.
  var legacyUrl = localStorage.getItem('romUrl') || '';
  return legacyUrl ? [{ name: 'ROM 1', url: legacyUrl }] : [];
}

function activeRomIndex(library) {
  var index = parseInt(localStorage.getItem('activeRomIndex') || '0', 10);
  return index >= 0 && index < library.length ? index : 0;
}

function settings() {
  var scaleMode = localStorage.getItem('scaleMode') || '1x';
  var library = romLibrary();
  var activeIndex = activeRomIndex(library);
  var activeRom = library.length ? library[activeIndex] : null;
  return {
    romLibrary: library,
    activeRomIndex: activeIndex,
    romUrl: activeRom ? activeRom.url : '',
    romName: activeRom ? activeRom.name : '',
    audioEnabled: localStorage.getItem('audioEnabled') === '1',
    scaleMode: (scaleMode === 'fullscreen' || scaleMode === 'fit') ? scaleMode : '1x'
  };
}

function pumpAppMessageQueue() {
  if (appMessageBusy || !appMessageQueue.length) {
    return;
  }

  appMessageBusy = true;
  var item = appMessageQueue[0];
  Pebble.sendAppMessage(item.message, function() {
    appMessageQueue.shift();
    appMessageBusy = false;
    pumpAppMessageQueue();
  }, function() {
    item.retries++;
    appMessageBusy = false;
    if (item.retries > MAX_SEND_RETRIES) {
      console.log('pebbleboy: appmessage send failed permanently');
      appMessageQueue.shift();
      sendError('phone send failed');
      pumpAppMessageQueue();
      return;
    }
    console.log('pebbleboy: appmessage retry ' + item.retries);
    setTimeout(pumpAppMessageQueue, 100 * item.retries);
  });
}

function sendQueue(messages) {
  for (var i = 0; i < messages.length; i++) {
    appMessageQueue.push({ message: messages[i], retries: 0 });
  }
  pumpAppMessageQueue();
}

function sendError(status) {
  console.log('pebbleboy: ROM error ' + status);
  Pebble.sendAppMessage({ PB_CMD: CMD.ROM_ERROR, PB_STATUS: status }, null, null);
}

var BASE64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function bytesToBase64(bytes) {
  var out = '';
  for (var i = 0; i < bytes.length; i += 3) {
    var a = bytes[i];
    var hasB = i + 1 < bytes.length;
    var hasC = i + 2 < bytes.length;
    var b = hasB ? bytes[i + 1] : 0;
    var c = hasC ? bytes[i + 2] : 0;
    out += BASE64_CHARS[a >> 2];
    out += BASE64_CHARS[((a & 3) << 4) | (b >> 4)];
    out += hasB ? BASE64_CHARS[((b & 15) << 2) | (c >> 6)] : '=';
    out += hasC ? BASE64_CHARS[c & 63] : '=';
  }
  return out;
}

function base64ToBytes(text) {
  var clean = String(text).replace(/[^A-Za-z0-9+/=]/g, '');
  var padding = 0;
  if (clean.length && clean.charAt(clean.length - 1) === '=') {
    padding++;
  }
  if (clean.length > 1 && clean.charAt(clean.length - 2) === '=') {
    padding++;
  }
  var out = new Uint8Array((clean.length / 4) * 3 - padding);
  var pos = 0;
  for (var i = 0; i < clean.length; i += 4) {
    var c0 = BASE64_CHARS.indexOf(clean.charAt(i));
    var c1 = BASE64_CHARS.indexOf(clean.charAt(i + 1));
    var c2 = clean.charAt(i + 2) === '=' ? 0 : BASE64_CHARS.indexOf(clean.charAt(i + 2));
    var c3 = clean.charAt(i + 3) === '=' ? 0 : BASE64_CHARS.indexOf(clean.charAt(i + 3));
    out[pos++] = (c0 << 2) | (c1 >> 4);
    if (pos < out.length) {
      out[pos++] = ((c1 & 15) << 4) | (c2 >> 2);
    }
    if (pos < out.length) {
      out[pos++] = ((c2 & 3) << 6) | c3;
    }
  }
  return out;
}

function crc32(bytes) {
  if (!crc32Table) {
    crc32Table = [];
    for (var i = 0; i < 256; i++) {
      var value = i;
      for (var bit = 0; bit < 8; bit++) {
        value = (value & 1) ? (0xEDB88320 ^ (value >>> 1)) : (value >>> 1);
      }
      crc32Table[i] = value >>> 0;
    }
  }
  var crc = 0xFFFFFFFF;
  for (var pos = 0; pos < bytes.length; pos++) {
    crc = crc32Table[(crc ^ bytes[pos]) & 0xFF] ^ (crc >>> 8);
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

function titleOf(bytes) {
  var title = '';
  for (var i = 0x134; i <= 0x143 && i < bytes.length; i++) {
    var c = bytes[i];
    if (c < 32 || c > 95) {
      break;
    }
    title += String.fromCharCode(c);
  }
  return title || 'DMG ROM';
}

function sha1(bytes) {
  function rol(n, b) { return (n << b) | (n >>> (32 - b)); }
  function hex(n) {
    var s = '';
    for (var i = 7; i >= 0; i--) {
      s += ((n >>> (i * 4)) & 0xF).toString(16);
    }
    return s;
  }
  var ml = bytes.length * 8;
  var withOne = bytes.length + 1;
  var paddedLen = withOne;
  while ((paddedLen % 64) !== 56) {
    paddedLen++;
  }
  var data = new Uint8Array(paddedLen + 8);
  data.set(bytes);
  data[bytes.length] = 0x80;
  var highLen = Math.floor(ml / 0x100000000);
  var lowLen = ml >>> 0;
  data[data.length - 8] = (highLen >>> 24) & 0xFF;
  data[data.length - 7] = (highLen >>> 16) & 0xFF;
  data[data.length - 6] = (highLen >>> 8) & 0xFF;
  data[data.length - 5] = highLen & 0xFF;
  data[data.length - 4] = (lowLen >>> 24) & 0xFF;
  data[data.length - 3] = (lowLen >>> 16) & 0xFF;
  data[data.length - 2] = (lowLen >>> 8) & 0xFF;
  data[data.length - 1] = lowLen & 0xFF;
  var h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  var w = new Array(80);
  for (var off = 0; off < data.length; off += 64) {
    for (var i = 0; i < 16; i++) {
      var p = off + i * 4;
      w[i] = (data[p] << 24) | (data[p + 1] << 16) | (data[p + 2] << 8) | data[p + 3];
    }
    for (i = 16; i < 80; i++) {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    var a = h0, b = h1, c = h2, d = h3, e = h4;
    for (i = 0; i < 80; i++) {
      var f, kk;
      if (i < 20) { f = (b & c) | ((~b) & d); kk = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; kk = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); kk = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; kk = 0xCA62C1D6; }
      var temp = (rol(a, 5) + f + e + kk + w[i]) | 0;
      e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }
    h0 = (h0 + a) | 0;
    h1 = (h1 + b) | 0;
    h2 = (h2 + c) | 0;
    h3 = (h3 + d) | 0;
    h4 = (h4 + e) | 0;
  }
  return hex(h0) + hex(h1) + hex(h2) + hex(h3) + hex(h4);
}

function loadCached(url) {
  try {
    var meta = JSON.parse(localStorage.getItem('romMeta') || 'null');
    if (!meta || meta.url !== url || !meta.chunks) {
      return false;
    }
    var out = new Uint8Array(meta.size);
    var pos = 0;
    for (var i = 0; i < meta.chunks; i++) {
      var part = base64ToBytes(localStorage.getItem('romChunk' + i) || '');
      var expected = Math.min(CACHE_CHUNK, meta.size - pos);
      if (part.length !== expected) {
        return false;
      }
      out.set(part, pos);
      pos += part.length;
    }
    if (pos !== meta.size) {
      return false;
    }
    romBytes = out;
    var actualCrc32 = crc32(out);
    if (typeof meta.crc32 === 'number' &&
        (meta.crc32 >>> 0) !== actualCrc32) {
      romBytes = null;
      return false;
    }
    if (typeof meta.crc32 !== 'number') {
      meta.crc32 = actualCrc32;
      localStorage.setItem('romMeta', JSON.stringify(meta));
    }
    romMeta = meta;
    return true;
  } catch (err) {
    console.log('pebbleboy: cache load failed: ' + err);
    return false;
  }
}

function saveCached(url, bytes, meta) {
  cacheSaveSerial++;
  var serial = cacheSaveSerial;
  var chunks = Math.ceil(bytes.length / CACHE_CHUNK);
  var cacheMeta = {
    size: meta.size,
    sha1: meta.sha1,
    crc32: meta.crc32,
    title: meta.title,
    cartType: meta.cartType,
    url: url,
    chunks: chunks
  };
  var i = 0;

  function saveNextChunk() {
    if (serial !== cacheSaveSerial) {
      return;
    }
    if (appMessageBusy || appMessageQueue.length) {
      setTimeout(saveNextChunk, CACHE_SAVE_IDLE_DELAY_MS);
      return;
    }
    try {
      if (i >= chunks) {
        localStorage.setItem('romMeta', JSON.stringify(cacheMeta));
        console.log('pebbleboy: cached ROM chunks=' + chunks);
        return;
      }
      localStorage.setItem('romChunk' + i,
        bytesToBase64(bytes.subarray(i * CACHE_CHUNK, Math.min((i + 1) * CACHE_CHUNK, bytes.length))));
      i++;
      setTimeout(saveNextChunk, CACHE_SAVE_CHUNK_DELAY_MS);
    } catch (err) {
      console.log('pebbleboy: cache save skipped: ' + err);
    }
  }

  setTimeout(saveNextChunk, CACHE_SAVE_INITIAL_DELAY_MS);
}

function clearCachedRom() {
  // Stop an incremental cache write before removing its chunks. Without this,
  // a delayed writer could repopulate storage after the user cleared it or
  // selected another ROM.
  cacheSaveSerial++;
  try {
    var meta = JSON.parse(localStorage.getItem('romMeta') || 'null');
    if (meta && meta.chunks) {
      for (var i = 0; i < meta.chunks; i++) {
        localStorage.removeItem('romChunk' + i);
      }
    }
    localStorage.removeItem('romMeta');
  } catch (err) {
    localStorage.removeItem('romMeta');
  }
  romBytes = null;
  romMeta = null;
}

function bytesFromFetchText(text) {
  var clean = String(text).replace(/\s+/g, '');
  if (clean.length >= 4 && clean.length % 4 === 0 &&
      /^[A-Za-z0-9+/]+={0,2}$/.test(clean)) {
    var decoded = base64ToBytes(clean);
    if (decoded.length >= 0x150) {
      return decoded;
    }
  }
  var bytes = new Uint8Array(text.length);
  for (var i = 0; i < text.length; i++) {
    bytes[i] = text.charCodeAt(i) & 0xFF;
  }
  return bytes;
}

function looksLikeTextPayload(bytes) {
  // A cartridge header contains binary opcodes and Nintendo logo data. Raw
  // base64/paste pages are printable ASCII, so inspecting a small prefix lets
  // us choose responseText without materialising a second full-size string.
  var size = Math.min(bytes.length, 512);
  for (var i = 0; i < size; i++) {
    var value = bytes[i];
    if (value !== 9 && value !== 10 && value !== 13 && (value < 32 || value > 126)) {
      return false;
    }
  }
  return size > 0;
}

function finishFetchRom(url, bytes, cb) {
  if (bytes.length < 32 * 1024 || bytes.length % BANK_SIZE !== 0) {
    cb('invalid ROM size');
    return;
  }
  if (bytes.length > MAX_ROM_SIZE) {
    cb('ROM exceeds 8 MB');
    return;
  }
  console.log('pebbleboy: fetched ' + bytes.length + ' bytes');
  var meta = {
    size: bytes.length,
    sha1: sha1(bytes),
    crc32: crc32(bytes),
    title: titleOf(bytes),
    cartType: bytes[0x147],
    url: url
  };
  romBytes = bytes;
  romMeta = meta;
  console.log('pebbleboy: ROM parsed ' + meta.title + ' sha1=' + meta.sha1);
  cb(null);
  saveCached(url, bytes, meta);
}

function fetchRomText(url, cb) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  if (xhr.overrideMimeType) {
    xhr.overrideMimeType('text/plain; charset=x-user-defined');
  }
  xhr.timeout = 20000;
  xhr.onload = function() {
    try {
      if (xhr.status !== 200) {
        cb('HTTP ' + xhr.status);
        return;
      }
      finishFetchRom(url, bytesFromFetchText(xhr.responseText || ''), cb);
    } catch (err) {
      cb('ROM parse failed: ' + err);
    }
  };
  xhr.onerror = xhr.ontimeout = function() { cb('ROM fetch failed'); };
  xhr.send();
}

function fetchRom(url, cb) {
  console.log('pebbleboy: fetching ROM ' + url);
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  try {
    xhr.responseType = 'arraybuffer';
  } catch (err) {
    fetchRomText(url, cb);
    return;
  }
  xhr.timeout = 20000;
  xhr.onload = function() {
    try {
      if (xhr.status !== 200) {
        cb('HTTP ' + xhr.status);
        return;
      }
      if (!xhr.response) {
        console.log('pebbleboy: arraybuffer empty, retrying as text');
        fetchRomText(url, cb);
        return;
      }
      var bytes = new Uint8Array(xhr.response);
      var contentType = xhr.getResponseHeader ? (xhr.getResponseHeader('Content-Type') || '') : '';
      if (bytes.length < 0x150 || /^text\//i.test(contentType) || looksLikeTextPayload(bytes)) {
        console.log('pebbleboy: text ROM response, retrying as text');
        fetchRomText(url, cb);
        return;
      }
      finishFetchRom(url, bytes, cb);
    } catch (err) {
      cb('ROM parse failed: ' + err);
    }
  };
  xhr.onerror = xhr.ontimeout = function() { cb('ROM fetch failed'); };
  xhr.send();
}

function ensureRom(cb) {
  var url = settings().romUrl;
  if (!url) {
    cb('NO_PHONE_ROM');
    return;
  }
  if (romBytes && romMeta && romMeta.url === url) {
    cb(null);
    return;
  }
  if (loadCached(url)) {
    cb(null);
    return;
  }
  if (romLoading && romLoadUrl === url) {
    romLoadCallbacks.push(cb);
    return;
  }

  romLoading = true;
  romLoadUrl = url;
  romLoadCallbacks = [cb];
  fetchRom(url, function(err) {
    var callbacks = romLoadCallbacks;
    romLoading = false;
    romLoadUrl = null;
    romLoadCallbacks = [];
    for (var i = 0; i < callbacks.length; i++) {
      callbacks[i](err);
    }
  });
}

function sendInfo() {
  var requestedUrl = settings().romUrl;
  if (romInfoPending && romInfoPendingUrl === requestedUrl) {
    console.log('pebbleboy: coalesced duplicate ROM info request');
    return;
  }
  romInfoPending = true;
  romInfoPendingUrl = requestedUrl;

  function clearPendingInfo() {
    if (romInfoPendingUrl === requestedUrl) {
      romInfoPending = false;
      romInfoPendingUrl = null;
    }
  }

  ensureRom(function(err) {
    if (err) {
      clearPendingInfo();
      sendError(err);
      return;
    }
    var cfg = settings();
    Pebble.sendAppMessage({
      PB_CMD: CMD.ROM_INFO,
      PB_SIZE: romMeta.size,
      PB_TITLE: romMeta.title,
      PB_CART_TYPE: romMeta.cartType,
      PB_CRC32: romMeta.crc32,
      PB_AUDIO: cfg.audioEnabled ? 1 : 0,
      PB_SCALE: cfg.scaleMode === 'fullscreen' ? 1 : (cfg.scaleMode === 'fit' ? 2 : 0)
    }, function() {
      setTimeout(clearPendingInfo, ROM_INFO_COALESCE_MS);
    }, function() {
      clearPendingInfo();
      console.log('pebbleboy: info send failed');
    });
    console.log('pebbleboy: ROM info ' + romMeta.title + ' size=' + romMeta.size);
  });
}

function sendInstallAtOffset(offset) {
  ensureRom(function(err) {
    if (err) {
      sendError(err);
      return;
    }
    offset = Number(offset);
    if (offset !== Math.floor(offset) || offset < 0 ||
        offset > romBytes.length || offset % MSG_CHUNK !== 0) {
      sendError('invalid install offset');
      return;
    }
    if (offset === romBytes.length) {
      console.log('pebbleboy: install payload sent, awaiting verification');
      sendQueue([{
        PB_CMD: CMD.ROM_INSTALL_END,
        PB_CRC32: romMeta.crc32
      }]);
      return;
    }
    var end = Math.min(offset + MSG_CHUNK, romBytes.length);
    sendQueue([{
      PB_CMD: CMD.ROM_INSTALL_DATA,
      PB_OFFSET: offset,
      PB_DATA: Array.prototype.slice.call(romBytes.subarray(offset, end))
    }]);
  });
}

function sendRomList() {
  var library = romLibrary();
  var messages = [{ PB_CMD: CMD.ROM_LIST_BEGIN, PB_SIZE: library.length }];
  for (var i = 0; i < library.length; i++) {
    messages.push({
      PB_CMD: CMD.ROM_LIST_ITEM,
      PB_BANK: i,
      PB_TITLE: (library[i].name || ('ROM ' + (i + 1))).slice(0, 16)
    });
  }
  messages.push({ PB_CMD: CMD.ROM_LIST_END, PB_SIZE: library.length });
  console.log('pebbleboy: ROM library entries=' + library.length);
  sendQueue(messages);
}

function selectRom(index) {
  var library = romLibrary();
  if (index < 0 || index >= library.length) {
    sendError('ROM selection invalid');
    return;
  }

  var previousUrl = settings().romUrl;
  var nextUrl = library[index].url;
  if (previousUrl !== nextUrl) {
    clearCachedRom();
  } else {
    romBytes = null;
    romMeta = null;
  }
  localStorage.setItem('activeRomIndex', String(index));
  localStorage.setItem('romLibrary', JSON.stringify(library));
  localStorage.removeItem('romUrl');
  console.log('pebbleboy: selected ROM ' + index + ' ' + library[index].name);
  sendInfo();
}

Pebble.addEventListener('appmessage', function(e) {
  var p = e.payload;
  if (p.PB_CMD === CMD.ROM_INFO_REQUEST) {
    sendInfo();
  } else if (p.PB_CMD === CMD.ROM_LIST_REQUEST) {
    sendRomList();
  } else if (p.PB_CMD === CMD.ROM_SELECT) {
    selectRom(p.PB_BANK | 0);
  } else if (p.PB_CMD === CMD.ROM_INSTALL_ACK) {
    sendInstallAtOffset(p.PB_OFFSET);
  } else if (p.PB_CMD === CMD.ROM_INSTALL_DONE) {
    console.log('pebbleboy: ROM installed and verified on watch');
  }
});

Pebble.addEventListener('ready', function() {
  console.log('pebbleboy phone service ready');
});

function configUrl() {
  var cfg = settings();
  return CONFIG_URL + '#' + encodeURIComponent(JSON.stringify({
    roms: cfg.romLibrary,
    scaleMode: cfg.scaleMode,
    audio: cfg.audioEnabled
  }));
}

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(configUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) {
    return;
  }
  try {
    var cfg = JSON.parse(decodeURIComponent(e.response));
    var oldSettings = settings();
    var library = normalizeRomLibrary(cfg.roms);
    var nextActiveIndex = 0;
    for (var i = 0; i < library.length; i++) {
      if (library[i].url === oldSettings.romUrl) {
        nextActiveIndex = i;
        break;
      }
    }
    var nextUrl = library.length ? library[nextActiveIndex].url : '';
    if (cfg.clear || oldSettings.romUrl !== nextUrl) {
      clearCachedRom();
    } else {
      romBytes = null;
      romMeta = null;
    }
    localStorage.setItem('romLibrary', JSON.stringify(library));
    localStorage.setItem('activeRomIndex', String(nextActiveIndex));
    localStorage.removeItem('romUrl');
    localStorage.setItem('scaleMode',
                         (cfg.scaleMode === 'fullscreen' || cfg.scaleMode === 'fit') ?
                         cfg.scaleMode : '1x');
    localStorage.setItem('audioEnabled', cfg.audio === false ? '0' : '1');
  } catch (err) {
    console.log('pebbleboy: bad config response');
  }
});
