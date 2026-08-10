#!/usr/bin/env node
const fs = require('fs');
const vm = require('vm');

const source = fs.readFileSync('src/pkjs/index.js', 'utf8');
const sent = [];
const listeners = {};
let openedUrl = '';
const store = new Map([
  ['romLibrary', JSON.stringify([
    { name: 'Tetris', url: 'https://example.invalid/tetris.gb' },
    { name: 'Pokemon Red', url: 'https://example.invalid/red.gb' },
  ])],
  ['activeRomIndex', '1'],
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
    removeItem(key) {
      store.delete(key);
    },
  },
  setTimeout(fn) {
    fn();
    return 1;
  },
  Pebble: {
    addEventListener(name, callback) {
      listeners[name] = callback;
    },
    openURL(url) {
      openedUrl = url;
    },
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
vm.runInContext(source, sandbox);

const cfg = sandbox.settings();
expect(cfg.romLibrary.length === 2, 'ROM library did not load both entries');
expect(cfg.activeRomIndex === 1 && cfg.romUrl.endsWith('/red.gb'),
       'active ROM selection was not restored');
listeners.showConfiguration();
expect(openedUrl.startsWith('https://ptv.netcavy.net/gb/?v=2#'),
       'configuration page did not use the hosted HTTPS URL');
const pageSettings = JSON.parse(decodeURIComponent(openedUrl.split('#')[1]));
expect(pageSettings.roms.length === 2 && pageSettings.roms[1].name === 'Pokemon Red',
       'configuration URL did not carry the existing ROM library');

const encodedRom = Buffer.alloc(0x150, 0x42).toString('base64');
const decodedRom = sandbox.bytesFromFetchText('PEBBLEBOY_ROM_BASE64\r\n' + encodedRom);
expect(decodedRom.length === 0x150 && decodedRom[0] === 0x42,
       'marked CRLF base64 ROM did not decode');
expect(sandbox.looksLikeTextPayload(new Uint8Array(Buffer.from(encodedRom, 'ascii'))),
       'base64 ArrayBuffer was not detected as text');
expect(!sandbox.looksLikeTextPayload(new Uint8Array([0, 0xc3, 0x50, 0x01])),
       'binary ROM was incorrectly detected as text');

sandbox.sendRomList();
expect(sent.length === 4, 'ROM library should send begin, two items and end');
expect(sent[0].PB_CMD === sandbox.CMD.ROM_LIST_BEGIN && sent[0].PB_SIZE === 2,
       'ROM library begin message mismatch');
expect(sent[1].PB_CMD === sandbox.CMD.ROM_LIST_ITEM && sent[1].PB_TITLE === 'Tetris',
       'first ROM library item mismatch');
expect(sent[2].PB_CMD === sandbox.CMD.ROM_LIST_ITEM && sent[2].PB_TITLE === 'Pokemon Red',
       'second ROM library item mismatch');
expect(sent[3].PB_CMD === sandbox.CMD.ROM_LIST_END && sent[3].PB_SIZE === 2,
       'ROM library end message mismatch');

store.delete('romLibrary');
store.delete('activeRomIndex');
store.set('romUrl', 'https://example.invalid/legacy.gb');
const legacy = sandbox.settings();
expect(legacy.romLibrary.length === 1 && legacy.romUrl.endsWith('/legacy.gb'),
       'legacy single-ROM setting was not migrated in memory');

console.log('pkjs ROM library test passed');
