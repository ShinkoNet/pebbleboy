#!/usr/bin/env node
const fs = require('fs');
const vm = require('vm');

const source = fs.readFileSync('src/pkjs/index.js', 'utf8');
const prefix = source.split("Pebble.addEventListener('appmessage'")[0];
const sent = [];
const timers = [];
const store = new Map([
  ['romUrl', 'http://example.invalid/install.gb'],
]);

const sandbox = {
  Array,
  JSON,
  Math,
  Number,
  String,
  Uint8Array,
  console,
  localStorage: {
    getItem(key) {
      return store.has(key) ? store.get(key) : null;
    },
    setItem(key, value) {
      store.set(key, String(value));
    },
    removeItem(key) {
      store.delete(key);
    },
  },
  setTimeout(fn) {
    timers.push(fn);
    return timers.length;
  },
  Pebble: {
    sendAppMessage(message, success) {
      sent.push(message);
      if (success) {
        success();
      }
    },
  },
};

function expect(condition, message) {
  if (!condition) {
    console.error(message);
    process.exit(1);
  }
}

vm.createContext(sandbox);
vm.runInContext(prefix, sandbox);

expect(sandbox.MAX_ROM_SIZE === 8 * 1024 * 1024, '8 MB ROM limit mismatch');

const crcVector = new Uint8Array([49, 50, 51, 52, 53, 54, 55, 56, 57]);
expect(sandbox.crc32(crcVector) === 0xCBF43926, 'CRC32 test vector mismatch');

const bytes = new Uint8Array(1024);
for (let i = 0; i < bytes.length; i++) {
  bytes[i] = i & 0xff;
}
sandbox.romBytes = bytes;
sandbox.romMeta = {
  size: bytes.length,
  sha1: '0123456789abcdef0123456789abcdef01234567',
  crc32: sandbox.crc32(bytes),
  title: 'INSTALL TEST',
  cartType: 0,
  url: store.get('romUrl'),
};

const ensureRom = sandbox.ensureRom;
const ensureCallbacks = [];
sandbox.ensureRom = function(cb) {
  ensureCallbacks.push(cb);
};
sandbox.sendInfo();
sandbox.sendInfo();
expect(ensureCallbacks.length === 1,
       'concurrent ROM info requests should share one load');
ensureCallbacks[0](null);
expect(sent.length === 1 && sent[0].PB_CMD === sandbox.CMD.ROM_INFO,
       'coalesced ROM info request did not send one response');
sandbox.sendInfo();
expect(sent.length === 1,
       'ROM info retry was not suppressed during delivery grace period');
expect(timers.length === 1, 'ROM info coalescing grace timer was not armed');
sandbox.ensureRom = ensureRom;
sent.length = 0;

sandbox.sendInstallAtOffset(0);
expect(sent.length === 1, 'first ACK should schedule exactly one chunk');
expect(sent[0].PB_CMD === sandbox.CMD.ROM_INSTALL_DATA,
       'first install message should contain data');
expect(sent[0].PB_OFFSET === 0 && sent[0].PB_DATA.length === sandbox.MSG_CHUNK,
       'first install chunk mismatch');

sandbox.sendInstallAtOffset(sandbox.MSG_CHUNK);
expect(sent.length === 2, 'second ACK should schedule exactly one more chunk');
expect(sent[1].PB_OFFSET === sandbox.MSG_CHUNK &&
       sent[1].PB_DATA[0] === 0 && sent[1].PB_DATA[1] === 1,
       'second install chunk mismatch');

sandbox.sendInstallAtOffset(bytes.length);
expect(sent.length === 3, 'final ACK should schedule exactly one end message');
expect(sent[2].PB_CMD === sandbox.CMD.ROM_INSTALL_END &&
       sent[2].PB_CRC32 === sandbox.romMeta.crc32,
       'install end CRC mismatch');

console.log('pkjs ROM install protocol test passed');
