var CMD = {
  ROM_INFO_REQUEST: 10,
  ROM_INFO: 11,
  ROM_BANK_REQUEST: 12,
  ROM_BANK_BEGIN: 13,
  ROM_BANK_DATA: 14,
  ROM_BANK_END: 15,
  ROM_ERROR: 16,
  SRAM_LOAD_REQUEST: 20,
  SRAM_LOAD_DATA: 21,
  SRAM_SAVE: 22
};

var BANK_SIZE = 16 * 1024;
var MSG_CHUNK = 512;
var CACHE_CHUNK = 8192;
var CACHE_SAVE_INITIAL_DELAY_MS = 5000;
var CACHE_SAVE_IDLE_DELAY_MS = 250;
var CACHE_SAVE_CHUNK_DELAY_MS = 25;
var SRAM_PAGE_SIZE = 4096;
var MAX_SEND_RETRIES = 5;
var romBytes = null;
var romMeta = null;
var appMessageQueue = [];
var appMessageBusy = false;
var romLoading = false;
var romLoadUrl = null;
var romLoadCallbacks = [];
var cacheSaveSerial = 0;

function settings() {
  var scaleMode = localStorage.getItem('scaleMode') || '1x';
  return {
    romUrl: localStorage.getItem('romUrl') || '',
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
      out.set(part, pos);
      pos += part.length;
    }
    romBytes = out;
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

function sramKey(parts) {
  if (!romMeta || !romMeta.sha1) {
    return null;
  }
  return ['sram', romMeta.sha1].concat(parts).join(':');
}

function payloadToBytes(payload) {
  if (!payload) {
    return new Uint8Array(0);
  }
  if (payload instanceof Uint8Array) {
    return payload;
  }
  return new Uint8Array(payload);
}

function isAllFF(bytes) {
  for (var i = 0; i < bytes.length; i++) {
    if (bytes[i] !== 0xFF) {
      return false;
    }
  }
  return true;
}

function loadSramChunk(bank, offset, len) {
  var key = sramKey(['bank', bank, 'chunk', Math.floor(offset / MSG_CHUNK)]);
  if (!key) {
    var missing = new Uint8Array(len);
    for (var m = 0; m < missing.length; m++) {
      missing[m] = 0xFF;
    }
    return missing;
  }
  var stored = localStorage.getItem(key);
  if (!stored) {
    var blank = new Uint8Array(len);
    for (var i = 0; i < blank.length; i++) {
      blank[i] = 0xFF;
    }
    return blank;
  }
  var bytes = base64ToBytes(stored);
  if (bytes.length === len) {
    return bytes;
  }
  var out = new Uint8Array(len);
  for (var i = 0; i < out.length; i++) {
    out[i] = 0xFF;
  }
  out.set(bytes.subarray(0, Math.min(bytes.length, len)));
  return out;
}

function saveSramChunk(bank, offset, totalSize, payload) {
  if (!romMeta || !romMeta.sha1) {
    return;
  }
  var bytes = payloadToBytes(payload);
  var metaKey = sramKey(['meta']);
  localStorage.setItem(metaKey, JSON.stringify({
    size: totalSize || 0,
    chunkSize: MSG_CHUNK
  }));

  var key = sramKey(['bank', bank, 'chunk', Math.floor(offset / MSG_CHUNK)]);
  if (isAllFF(bytes)) {
    localStorage.removeItem(key);
  } else {
    localStorage.setItem(key, bytesToBase64(bytes));
  }
}

function sendSramLoad(bank, requestSize) {
  ensureRom(function(err) {
    if (err) {
      sendError(err);
      return;
    }
    var size = requestSize || SRAM_PAGE_SIZE;
    var messages = [];
    for (var off = 0; off < size; off += MSG_CHUNK) {
      var len = Math.min(MSG_CHUNK, size - off);
      messages.push({
        PB_CMD: CMD.SRAM_LOAD_DATA,
        PB_BANK: bank,
        PB_OFFSET: off,
        PB_DATA: Array.prototype.slice.call(loadSramChunk(bank, off, len))
      });
    }
    console.log('pebbleboy: SRAM load bank ' + bank + ' size=' + size);
    sendQueue(messages);
  });
}

function bytesFromFetchText(text) {
  var marker = 'PEBBLEBOY_ROM_BASE64\n';
  if (text.indexOf(marker) === 0) {
    return base64ToBytes(text.slice(marker.length));
  }
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

function finishFetchRom(url, bytes, cb) {
  if (bytes.length < 0x150) {
    cb('ROM too small');
    return;
  }
  console.log('pebbleboy: fetched ' + bytes.length + ' bytes');
  var meta = {
    size: bytes.length,
    sha1: sha1(bytes),
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
      if (bytes.length < 0x150) {
        console.log('pebbleboy: arraybuffer too small, retrying as text');
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
  ensureRom(function(err) {
    if (err) {
      sendError(err);
      return;
    }
    var cfg = settings();
    Pebble.sendAppMessage({
      PB_CMD: CMD.ROM_INFO,
      PB_SIZE: romMeta.size,
      PB_TITLE: romMeta.title,
      PB_CART_TYPE: romMeta.cartType,
      PB_SHA1: romMeta.sha1,
      PB_AUDIO: cfg.audioEnabled ? 1 : 0,
      PB_SCALE: cfg.scaleMode === 'fullscreen' ? 1 : (cfg.scaleMode === 'fit' ? 2 : 0)
    }, null, function() { console.log('pebbleboy: info send failed'); });
    console.log('pebbleboy: ROM info ' + romMeta.title + ' size=' + romMeta.size);
  });
}

function sendBank(bank, bankOffset, requestSize) {
  ensureRom(function(err) {
    if (err) {
      sendError(err);
      return;
    }
    bankOffset = bankOffset || 0;
    requestSize = requestSize || BANK_SIZE;
    var start = bank * BANK_SIZE + bankOffset;
    if (start >= romBytes.length) {
      sendError('bank outside ROM');
      return;
    }
    var size = Math.min(requestSize, BANK_SIZE - bankOffset, romBytes.length - start);
    console.log('pebbleboy: bank request ' + bank + ' offset=' + bankOffset + ' size=' + size);
    var messages = [{
      PB_CMD: CMD.ROM_BANK_BEGIN,
      PB_BANK: bank,
      PB_OFFSET: bankOffset,
      PB_SIZE: size
    }];
    for (var off = 0; off < size; off += MSG_CHUNK) {
      var end = Math.min(off + MSG_CHUNK, size);
      messages.push({
        PB_CMD: CMD.ROM_BANK_DATA,
        PB_BANK: bank,
        PB_OFFSET: bankOffset + off,
        PB_DATA: Array.prototype.slice.call(romBytes.subarray(start + off, start + end))
      });
    }
    messages.push({
      PB_CMD: CMD.ROM_BANK_END,
      PB_BANK: bank,
      PB_OFFSET: bankOffset,
      PB_SIZE: size,
      PB_SHA1: romMeta.sha1
    });
    sendQueue(messages);
  });
}

Pebble.addEventListener('appmessage', function(e) {
  var p = e.payload;
  if (p.PB_CMD === CMD.ROM_INFO_REQUEST) {
    sendInfo();
  } else if (p.PB_CMD === CMD.ROM_BANK_REQUEST) {
    sendBank(p.PB_BANK | 0, p.PB_OFFSET | 0, p.PB_SIZE | 0);
  } else if (p.PB_CMD === CMD.SRAM_LOAD_REQUEST) {
    sendSramLoad(p.PB_BANK | 0, p.PB_SIZE | 0);
  } else if (p.PB_CMD === CMD.SRAM_SAVE) {
    saveSramChunk(p.PB_BANK | 0, p.PB_OFFSET | 0, p.PB_SRAM_SIZE | 0, p.PB_DATA);
  }
});

Pebble.addEventListener('ready', function() {
  console.log('pebbleboy phone service ready');
});

function htmlAttr(text) {
  return String(text || '').replace(/&/g, '&amp;').replace(/"/g, '&quot;')
    .replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function configHtml() {
  var cfg = settings();
  return '<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width">' +
    '<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:0;' +
    'background:#f6f7f9;color:#111}main{padding:18px}h1{font-size:22px;margin:0 0 16px}' +
    'label{display:block;font-size:13px;font-weight:600;margin:14px 0 6px}' +
    'input[type=url],input[type=text],select{width:100%;padding:12px;border:1px solid #c9ced6;' +
    'border-radius:6px;box-sizing:border-box;font-size:15px;background:white}' +
    '.hint{font-size:12px;line-height:1.35;color:#4b5563;margin:8px 0 0}' +
    '.row{display:flex;gap:10px;align-items:center;margin:16px 0}.row input{width:auto}' +
    'button{width:100%;padding:13px;background:#111827;color:white;border:0;border-radius:6px;' +
    'font-size:16px;font-weight:600}</style></head><body><main><h1>Pebbleboy</h1>' +
    '<label for="u">ROM URL</label>' +
    '<input id="u" type="url" inputmode="url" placeholder="https://pastebin.com/raw/..." ' +
    'value="' + htmlAttr(cfg.romUrl) + '">' +
    '<p class="hint">Use a direct .gb URL, a raw Pastebin URL containing base64, or text starting ' +
    'with PEBBLEBOY_ROM_BASE64.</p>' +
    '<label for="s">Display scale</label>' +
    '<select id="s">' +
    '<option value="1x"' + (cfg.scaleMode === '1x' ? ' selected' : '') + '>1:1 centered</option>' +
    '<option value="fullscreen"' + (cfg.scaleMode === 'fullscreen' ? ' selected' : '') +
    '>Fill screen</option>' +
    '<option value="fit"' + (cfg.scaleMode === 'fit' ? ' selected' : '') +
    '>Aspect fit</option>' +
    '</select>' +
    '<label class="row"><input id="a" type="checkbox" ' +
    (cfg.audioEnabled ? 'checked' : '') + '> Enable speaker audio</label>' +
    '<label class="row"><input id="c" type="checkbox"> Clear cached ROM after save</label>' +
    '<button onclick="done()">Save</button></main>' +
    '<script>function done(){location.href="pebblejs://close#"+encodeURIComponent(' +
    'JSON.stringify({romUrl:document.getElementById("u").value.trim(),' +
    'scaleMode:document.getElementById("s").value,' +
    'audio:document.getElementById("a").checked,' +
    'clear:document.getElementById("c").checked}))}</' + 'script></body></html>';
}

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(configHtml()));
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) {
    return;
  }
  try {
    var cfg = JSON.parse(decodeURIComponent(e.response));
    var oldUrl = settings().romUrl;
    localStorage.setItem('romUrl', cfg.romUrl || '');
    localStorage.setItem('scaleMode',
                         (cfg.scaleMode === 'fullscreen' || cfg.scaleMode === 'fit') ?
                         cfg.scaleMode : '1x');
    localStorage.setItem('audioEnabled', cfg.audio === false ? '0' : '1');
    if (cfg.clear || oldUrl !== (cfg.romUrl || '')) {
      clearCachedRom();
    } else {
      romBytes = null;
      romMeta = null;
    }
  } catch (err) {
    console.log('pebbleboy: bad config response');
  }
});
