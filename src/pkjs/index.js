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
var MAX_SEND_RETRIES = 5;
var romBytes = null;
var romMeta = null;
var appMessageQueue = [];
var appMessageBusy = false;
var romLoading = false;
var romLoadUrl = null;
var romLoadCallbacks = [];

function settings() {
  return {
    romUrl: localStorage.getItem('romUrl') || ''
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
  for (var k = 0; k < 8; k++) {
    data[data.length - 1 - k] = (ml >>> (k * 8)) & 0xFF;
  }
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
    try {
      if (i >= chunks) {
        localStorage.setItem('romMeta', JSON.stringify(cacheMeta));
        console.log('pebbleboy: cached ROM chunks=' + chunks);
        return;
      }
      localStorage.setItem('romChunk' + i,
        bytesToBase64(bytes.subarray(i * CACHE_CHUNK, Math.min((i + 1) * CACHE_CHUNK, bytes.length))));
      i++;
      setTimeout(saveNextChunk, 0);
    } catch (err) {
      console.log('pebbleboy: cache save skipped: ' + err);
    }
  }

  setTimeout(saveNextChunk, 0);
}

function bytesFromFetchText(text) {
  var marker = 'PEBBLEBOY_ROM_BASE64\n';
  if (text.indexOf(marker) === 0) {
    return base64ToBytes(text.slice(marker.length));
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
    Pebble.sendAppMessage({
      PB_CMD: CMD.ROM_INFO,
      PB_SIZE: romMeta.size,
      PB_TITLE: romMeta.title,
      PB_CART_TYPE: romMeta.cartType,
      PB_SHA1: romMeta.sha1
    }, null, function() { console.log('pebbleboy: info send failed'); });
    console.log('pebbleboy: ROM info ' + romMeta.title + ' size=' + romMeta.size);
  });
}

function sendBank(bank) {
  ensureRom(function(err) {
    if (err) {
      sendError(err);
      return;
    }
    var start = bank * BANK_SIZE;
    if (start >= romBytes.length) {
      sendError('bank outside ROM');
      return;
    }
    var size = Math.min(BANK_SIZE, romBytes.length - start);
    console.log('pebbleboy: bank request ' + bank + ' size=' + size);
    var messages = [{ PB_CMD: CMD.ROM_BANK_BEGIN, PB_BANK: bank, PB_SIZE: size }];
    for (var off = 0; off < size; off += MSG_CHUNK) {
      var end = Math.min(off + MSG_CHUNK, size);
      messages.push({
        PB_CMD: CMD.ROM_BANK_DATA,
        PB_BANK: bank,
        PB_OFFSET: off,
        PB_DATA: Array.prototype.slice.call(romBytes.subarray(start + off, start + end))
      });
    }
    messages.push({
      PB_CMD: CMD.ROM_BANK_END,
      PB_BANK: bank,
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
    sendBank(p.PB_BANK | 0);
  }
});

Pebble.addEventListener('ready', function() {
  console.log('pebbleboy phone service ready');
});

var CONFIG_HTML =
  '<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width">' +
  '<style>body{font-family:sans-serif;margin:1em;background:#f7f7f7;color:#111}' +
  'input{width:100%;padding:.6em;margin:.4em 0 1em;box-sizing:border-box}' +
  'button{width:100%;padding:.8em;background:#111;color:white;border:0;font-size:1em}</style>' +
  '</head><body><h3>Pebbleboy</h3>' +
  '<label>Phone-backed ROM URL</label>' +
  '<input id="u" placeholder="https://example.test/rom.gb" value="__URL__">' +
  '<button onclick="done()">Save</button>' +
  '<script>function done(){location.href="pebblejs://close#"+encodeURIComponent(' +
  'JSON.stringify({romUrl:document.getElementById("u").value.trim()}))}</' +
  'script></body></html>';

Pebble.addEventListener('showConfiguration', function() {
  var html = CONFIG_HTML.replace('__URL__', settings().romUrl);
  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) {
    return;
  }
  try {
    var cfg = JSON.parse(decodeURIComponent(e.response));
    localStorage.setItem('romUrl', cfg.romUrl || '');
    romBytes = null;
    romMeta = null;
  } catch (err) {
    console.log('pebbleboy: bad config response');
  }
});
