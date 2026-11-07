import assert from 'node:assert/strict';
import { test } from 'node:test';
import * as lib from './index.js';

test('HOST-LIB-021 the package exports both clients, detection, attention and color helpers', () => {
  for (const name of [
    'ClocTeckClient', 'AttentionController', 'parseColor', 'NAMED_COLORS',
    'TubeClockClient', 'AuthError', 'ValidationError', 'HttpError', 'detectFirmware',
  ]) {
    assert.ok(name in lib, name);
  }
});
