#!/usr/bin/env node
const fs = require('fs');
const vm = require('vm');

const source = fs.readFileSync('src/pkjs/index.js', 'utf8');
const prefix = source.split('function clearCachedRom')[0];
const timers = [];
const store = new Map();
const writes = [];
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
      writes.push(key);
    },
  },
  setTimeout(fn, delay) {
    timers.push({ fn, delay });
    return timers.length;
  },
};

function expect(condition, message) {
  if (!condition) {
    console.error(message);
    process.exit(1);
  }
}

function runNextTimer() {
  expect(timers.length > 0, 'expected a scheduled timer');
  return timers.shift().fn();
}

vm.createContext(sandbox);
vm.runInContext(prefix, sandbox);

const bytes = new Uint8Array(20000);
for (let i = 0; i < bytes.length; i++) {
  bytes[i] = i & 0xff;
}
const meta = {
  size: bytes.length,
  sha1: '0123456789abcdef0123456789abcdef01234567',
  title: 'CACHE TEST',
  cartType: 0,
};

sandbox.saveCached('http://example.invalid/cache.gb', bytes, meta);
expect(timers.length === 1, 'cache save should schedule one initial timer');
expect(timers[0].delay === sandbox.CACHE_SAVE_INITIAL_DELAY_MS,
       'cache save did not use the initial delay');
expect(writes.length === 0, 'cache save wrote before the initial timer');

sandbox.appMessageBusy = true;
runNextTimer();
expect(writes.length === 0, 'cache save wrote while AppMessage was busy');
expect(timers[0].delay === sandbox.CACHE_SAVE_IDLE_DELAY_MS,
       'busy cache save did not retry with idle delay');

sandbox.appMessageBusy = false;
sandbox.appMessageQueue.push({ message: {}, retries: 0 });
runNextTimer();
expect(writes.length === 0, 'cache save wrote while AppMessage queue was non-empty');
expect(timers[0].delay === sandbox.CACHE_SAVE_IDLE_DELAY_MS,
       'queued cache save did not retry with idle delay');

sandbox.appMessageQueue.length = 0;
runNextTimer();
expect(writes[0] === 'romChunk0', 'cache save did not write the first ROM chunk first');
expect(timers[0].delay === sandbox.CACHE_SAVE_CHUNK_DELAY_MS,
       'cache save did not throttle chunk writes');

while (timers.length) {
  runNextTimer();
}

expect(store.has('romMeta'), 'cache save did not commit metadata after chunks');
const cacheMeta = JSON.parse(store.get('romMeta'));
expect(cacheMeta.chunks === Math.ceil(bytes.length / sandbox.CACHE_CHUNK),
       'cache metadata chunk count mismatch');
console.log('pkjs cache scheduler test passed');
