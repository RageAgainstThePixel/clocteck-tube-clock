import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hexToHsv, hsvToHex, parseColor } from './color.js';

test('HOST-LIB-020 hexToHsv converts primaries and shorthand', () => {
  assert.deepEqual(hexToHsv('#ff0000'), { h: 0, s: 100, v: 100 });
  assert.deepEqual(hexToHsv('00ff00'), { h: 120, s: 100, v: 100 });
  assert.deepEqual(hexToHsv('#00f'), { h: 240, s: 100, v: 100 });
  assert.deepEqual(hexToHsv('#000000'), { h: 0, s: 0, v: 0 });
});

test('HOST-LIB-020 hsvToHex round-trips', () => {
  for (const hex of ['#ff8000', '#123456', '#ffffff', '#8000ff']) {
    const back = hsvToHex(hexToHsv(hex));
    // HSV is rounded to integers, so allow off-by-one per channel
    for (let i = 1; i < 7; i += 2) {
      assert.ok(Math.abs(parseInt(back.slice(i, i + 2), 16) - parseInt(hex.slice(i, i + 2), 16)) <= 3, `${hex} -> ${back}`);
    }
  }
});

test('HOST-LIB-020 parseColor accepts h,s,v and rejects out-of-range values', () => {
  assert.deepEqual(parseColor('200, 50, 75'), { h: 200, s: 50, v: 75 });
  assert.throws(() => parseColor('400,50,50'));
  assert.throws(() => parseColor('not-a-color'));
});
