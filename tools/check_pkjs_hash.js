#!/usr/bin/env node
const crypto = require('crypto');
const fs = require('fs');
const vm = require('vm');

if (process.argv.length < 3) {
  console.error(`usage: ${process.argv[1]} ROM...`);
  process.exit(2);
}

const source = fs.readFileSync('src/pkjs/index.js', 'utf8');
const prefix = source.split('function loadCached')[0];
const sandbox = {
  Array,
  Math,
  String,
  Uint8Array,
  console,
  localStorage: { getItem() { return ''; } },
  setTimeout() {},
};
vm.createContext(sandbox);
vm.runInContext(prefix, sandbox);

let failed = false;
for (const path of process.argv.slice(2)) {
  const bytes = fs.readFileSync(path);
  const expected = crypto.createHash('sha1').update(bytes).digest('hex');
  const actual = sandbox.sha1(new Uint8Array(bytes));
  console.log(`pkjs sha1 ${path} ${actual}`);
  if (actual !== expected) {
    console.error(`expected ${expected}, saw ${actual}`);
    failed = true;
  }
}

process.exit(failed ? 1 : 0);
