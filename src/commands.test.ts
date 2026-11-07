import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  commandLabel,
  displayChange,
  effectBody,
  parsePalette,
  parsePulse,
  percentToBrightness,
  requiredFirmware,
  speedToPeriod,
  timeSynced,
  unsupportedHint,
  unsupportedMessage,
  utcOffsetToPosix,
} from './commands.js';
import { sampleState } from './fake-server.js';
import type { TubeClockState } from './tubeclock.js';

test('HOST-CLI-016 a whole-hour offset becomes a fixed POSIX rule', () => {
  assert.equal(utcOffsetToPosix(-5), '<-05>5');
  assert.equal(utcOffsetToPosix(3), '<+03>-3');
  assert.equal(utcOffsetToPosix(0), '<+00>0');
  assert.equal(utcOffsetToPosix(-12), '<-12>12');
  assert.equal(utcOffsetToPosix(12), '<+12>-12');
  assert.throws(() => utcOffsetToPosix(13), RangeError);
  assert.throws(() => utcOffsetToPosix(1.5), RangeError);
});

test('HOST-CLI-012 brightness percent maps to 0-255, rounded', () => {
  assert.equal(percentToBrightness(0), 0);
  assert.equal(percentToBrightness(50), 128);
  assert.equal(percentToBrightness(100), 255);
});

test('HOST-CLI-007 HOST-CLI-013..025 command matrix', () => {
  for (const cmd of ['mode', 'digits', 'time', 'wifi'] as const) assert.equal(requiredFirmware(cmd, [], false), 'stock', cmd);
  for (const cmd of ['tz', 'restart'] as const) assert.equal(requiredFirmware(cmd, [], false), 'tubeclock', cmd);
  for (const cmd of ['status', 'color', 'brightness', 'hours', 'timezone', 'sync', 'detect'] as const) {
    assert.equal(requiredFirmware(cmd, [], false), undefined, cmd);
  }
  assert.equal(requiredFirmware('firmware', [], false), undefined);
  assert.equal(requiredFirmware('firmware', [], true), 'stock');
  assert.equal(requiredFirmware('attention', ['red'], false), undefined);
  assert.equal(requiredFirmware('attention', ['cancel'], false), 'tubeclock');
  assert.equal(unsupportedMessage(commandLabel('firmware', [], true), 'stock'), '"firmware --update" needs the stock firmware');
  assert.equal(unsupportedMessage('restart', 'tubeclock'), '"restart" needs the new firmware');
});

test('HOST-CLI-026..028 the effects commands need the new firmware; HOST-CLI-013 mode says what to use', () => {
  for (const cmd of ['preset', 'effect', 'display'] as const) assert.equal(requiredFirmware(cmd, [], false), 'tubeclock', cmd);
  assert.match(unsupportedHint('mode', 'stock') ?? '', /preset use.*effect set/);
});

test('HOST-CLI-027 OQ-23 --speed is a log scale from 60 s to 600 ms', () => {
  assert.equal(speedToPeriod(1), 60000);
  assert.equal(speedToPeriod(100), 600);
  assert.equal(speedToPeriod(50), Math.round(60000 * 0.01 ** (49 / 99)));
  for (const bad of [0, 101, 1.5, NaN]) assert.throws(() => speedToPeriod(bad), RangeError);
});

test('HOST-CLI-027 --palette: names, hex, rainbow and h,s,v with its own commas', () => {
  assert.equal(parsePalette('rainbow'), 'rainbow');
  assert.deepEqual(parsePalette('red,#00f,#123456'), ['red', '#00f', '#123456']);
  assert.deepEqual(parsePalette('241,100,100'), ['241,100,100']);
  assert.deepEqual(parsePalette('red,241,100,100,0,0,50,#fff'), ['red', '241,100,100', '0,0,50', '#fff']);
  assert.deepEqual(parsePalette('123'), ['123']);  // too short for a triple: for the firmware to judge
});

test('HOST-CLI-027 --pulse is min,max[,ms]', () => {
  assert.deepEqual(parsePulse('64,255'), { min: 64, max: 255 });
  assert.deepEqual(parsePulse('64,255,4000'), { min: 64, max: 255, period: 4000 });
  assert.throws(() => parsePulse('64'), RangeError);
  assert.throws(() => parsePulse('1,2,3,4'), RangeError);
});

test('HOST-CLI-027 effect set sends only what was asked for', () => {
  assert.deepEqual(effectBody('wave', {}), { pattern: 'wave' });
  assert.deepEqual(
    effectBody('wave', { palette: 'red,blue', direction: 'rtl', period: '5000', phase: '20', drift: '60000', easing: 'sine', pulse: '1,2' }),
    { pattern: 'wave', palette: ['red', 'blue'], direction: 'rtl', period: 5000, phase: 20, drift: 60000, easing: 'sine', pulse: { min: 1, max: 2 } },
  );
  assert.deepEqual(effectBody('wave', { speed: '100' }), { pattern: 'wave', period: 600 });
  assert.throws(() => effectBody('wave', { speed: '10', period: '100' }), RangeError);
  // The v1.1 patterns' fields go along as numbers.
  assert.deepEqual(effectBody('chase', { density: '4', seed: '9', width: '2', count: '1', duty: '50' }), { pattern: 'chase', density: 4, seed: 9, width: 2, count: 1, duty: 50 });
  // FW-FX-016, FW-FX-017: accents and glyphs.
  assert.deepEqual(effectBody('matrix', { glyphs: 'on', accent: 'flicker,600,tubes=hm,color=#ff8000,spread=80,strength=70' }), {
    pattern: 'matrix',
    glyphs: true,
    accent: { style: 'flicker', ms: 600, tubes: 'hm', color: '#ff8000', spread: 80, strength: 70 },
  });
  assert.deepEqual(effectBody('wave', { accent: 'dip' }), { pattern: 'wave', accent: { style: 'dip' } });
  assert.deepEqual(effectBody('wave', { accent: 'off' }), { pattern: 'wave', accent: null });
  assert.throws(() => effectBody('wave', { accent: 'dip,400,500' }), RangeError);
  assert.throws(() => effectBody('wave', { accent: 'dip,speed=3' }), RangeError);
  assert.throws(() => effectBody('wave', { glyphs: 'yes' }), RangeError);
  // Not a number: sent as it came, for the firmware's 400 to name.
  assert.deepEqual(effectBody('wave', { period: 'soon' }), { pattern: 'wave', period: 'soon' });
});

test('HOST-CLI-028 display flags', () => {
  assert.deepEqual(displayChange({}), {});
  assert.deepEqual(displayChange({ hours: '24', leadingZero: 'off', seconds: 'on', transition: 'crossfade', fade: '300' }), {
    hours: 24,
    leadingZero: false,
    seconds: true,
    transition: 'crossfade',
    fadeMs: 300,
  });
  assert.deepEqual(displayChange({ tubeBrightness: '1,2,3,4,5,6' }), { tubeBrightness: [1, 2, 3, 4, 5, 6] });
  assert.throws(() => displayChange({ seconds: 'maybe' }), RangeError);
  assert.throws(() => displayChange({ hours: '13' }), RangeError);
});

test('HOST-CLI-019 timeSynced uses lastSync when reported, else a known time', () => {
  const state = (o: Record<string, unknown>) => sampleState(o) as TubeClockState;
  assert.equal(timeSynced(state({})), true);
  assert.equal(timeSynced(state({ time: 'unset' })), false);
  assert.equal(timeSynced(state({ lastSync: null })), false);
  assert.equal(timeSynced(state({ lastSync: '2026-11-03T12:00:00Z' })), true);
});
