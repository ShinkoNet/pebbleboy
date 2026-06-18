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
var MSG_CHUNK = 128;
var CACHE_CHUNK = 8192;
var romBytes = null;
var romMeta = null;

function settings() {
  return {
    romUrl: localStorage.getItem('romUrl') || ''
  };
}

function sendQueue(messages) {
  if (!messages.length) {
    return;
  }
  Pebble.sendAppMessage(messages.shift(), function() {
    sendQueue(messages);
  }, function() {
    console.log('pebbleboy: appmessage send failed');
  });
}

function sendError(status) {
  Pebble.sendAppMessage({ PB_CMD: CMD.ROM_ERROR, PB_STATUS: status }, null, null);
}

function bytesToBase64(bytes) {
  var out = '';
  for (var i = 0; i < bytes.length; i += 1024) {
    var part = bytes.subarray(i, Math.min(i + 1024, bytes.length));
    for (var j = 0; j < part.length; j++) {
      out += String.fromCharCode(part[j]);
    }
  }
  return btoa(out);
}

function base64ToBytes(text) {
  var bin = atob(text);
  var out = new Uint8Array(bin.length);
  for (var i = 0; i < bin.length; i++) {
    out[i] = bin.charCodeAt(i) & 0xFF;
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
  try {
    var chunks = Math.ceil(bytes.length / CACHE_CHUNK);
    for (var i = 0; i < chunks; i++) {
      localStorage.setItem('romChunk' + i,
        bytesToBase64(bytes.subarray(i * CACHE_CHUNK, Math.min((i + 1) * CACHE_CHUNK, bytes.length))));
    }
    meta.url = url;
    meta.chunks = chunks;
    localStorage.setItem('romMeta', JSON.stringify(meta));
  } catch (err) {
    console.log('pebbleboy: cache save skipped: ' + err);
  }
}

function fetchRom(url, cb) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.responseType = 'arraybuffer';
  xhr.timeout = 20000;
  xhr.onload = function() {
    if (xhr.status !== 200 || !xhr.response) {
      cb('HTTP ' + xhr.status);
      return;
    }
    var bytes = new Uint8Array(xhr.response);
    if (bytes.length < 0x150) {
      cb('ROM too small');
      return;
    }
    var meta = {
      size: bytes.length,
      sha1: sha1(bytes),
      title: titleOf(bytes),
      cartType: bytes[0x147]
    };
    romBytes = bytes;
    romMeta = meta;
    saveCached(url, bytes, meta);
    cb(null);
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
  fetchRom(url, cb);
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
