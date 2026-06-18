#!/usr/bin/env node
const fs = require('fs');
const vm = require('vm');

const source = fs.readFileSync('src/pkjs/index.js', 'utf8');
const prefix = source.split("Pebble.addEventListener('appmessage'")[0];
const sent = [];
const store = new Map([
  ['romUrl', 'http://example.invalid/test.gb'],
]);

const sandbox = {
  Array,
  JSON,
  Math,
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
  },
  setTimeout(fn) {
    fn();
    return 1;
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

const romSize = sandbox.BANK_SIZE * 2;
const bytes = new Uint8Array(romSize);
for (let i = 0; i < bytes.length; i++) {
  bytes[i] = i & 0xff;
}
sandbox.romBytes = bytes;
sandbox.romMeta = {
  size: romSize,
  sha1: '0123456789abcdef0123456789abcdef01234567',
  title: 'BANK TEST',
  cartType: 0,
  url: store.get('romUrl'),
};

sandbox.sendBank(1, sandbox.MSG_CHUNK, sandbox.MSG_CHUNK);
expect(sent.length === 1, 'small fill should use one AppMessage');
expect(sent[0].PB_CMD === sandbox.CMD.ROM_BANK_END,
       'small fill should complete with ROM_BANK_END');
expect(sent[0].PB_BANK === 1 && sent[0].PB_OFFSET === sandbox.MSG_CHUNK,
       'small fill bank/offset mismatch');
expect(sent[0].PB_SIZE === sandbox.MSG_CHUNK, 'small fill size mismatch');
expect(sent[0].PB_DATA.length === sandbox.MSG_CHUNK, 'small fill data length mismatch');
expect(sent[0].PB_DATA[0] === 0 && sent[0].PB_DATA[1] === 1,
       'small fill payload mismatch');

sent.length = 0;
sandbox.sendBank(0, 0, sandbox.BANK_SIZE);
expect(sent.length === 34, 'full bank should keep begin/data/end sequence');
expect(sent[0].PB_CMD === sandbox.CMD.ROM_BANK_BEGIN, 'full bank missing begin');
expect(sent[1].PB_CMD === sandbox.CMD.ROM_BANK_DATA, 'full bank missing data');
expect(sent[sent.length - 1].PB_CMD === sandbox.CMD.ROM_BANK_END,
       'full bank missing end');
console.log('pkjs bank scheduler test passed');
