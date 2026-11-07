// FW-OTA-008: writes one firmware image to the clock at address 0 over JP1, then checks it against the chip's own MD5.
//   npm run fw:flash -- --check <image>             validates the image and backup, touches no hardware
//   npm run fw:flash -- --write <image> [port]      really writes
// Without --write it opens no serial port. It refuses to write until an intact full backup is present (fw:backup).
// A 1 MB backup is itself a valid image, which is how the stock firmware goes back on.
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ESPLoader, Transport } from 'esptool-js';
import { NodeSerialDevice, waitForChip } from './serial-device.mjs';

const [mode, imagePath, port = 'COM4'] = process.argv.slice(2);
if ((mode !== '--check' && mode !== '--write') || !imagePath) {
  console.error('usage: npm run fw:flash -- --check|--write <image> [port]');
  process.exit(2);
}
const here = dirname(fileURLToPath(import.meta.url));
const backups = resolve(here, '..', '..', 'clock', 'backups');
const md5 = (data) => createHash('md5').update(data).digest('hex');

function checkBackup() {
  const names = existsSync(backups) ? readdirSync(backups).filter((f) => f.endsWith('.bin')) : [];
  if (names.length === 0) throw new Error('no backup in clock/backups: run npm run fw:backup first');
  for (const name of names) {
    const data = readFileSync(resolve(backups, name));
    const [expected] = readFileSync(resolve(backups, `${name}.sha256`), 'utf8').split(/\s+/);
    if (createHash('sha256').update(data).digest('hex') !== expected) throw new Error(`${name} does not match its sha256`);
    if (data.length !== 0x100000) throw new Error(`${name} is not a full 1 MB image`);
    console.log(`backup ok: ${name}`);
  }
}

function checkImage() {
  const data = new Uint8Array(readFileSync(imagePath));
  // A whole-flash restore is 1 MB; anything else has to fit the sketch slot (FW-BUD-001).
  if (data.length !== 0x100000 && data.length > 481280) throw new Error(`image is ${data.length} bytes, over the limit`);
  if (data[0] !== 0xe9) throw new Error('not an ESP image: first byte is not 0xE9');
  // Byte 2 is the flash mode. The ESP8285 needs DOUT (3) (FW-HW-009).
  if (data[2] !== 3) throw new Error(`flash mode is ${data[2]}, the ESP8285 needs DOUT (3)`);
  console.log(`image ok: ${imagePath}, ${data.length} bytes, md5 ${md5(data)}`);
  return data;
}

checkBackup();
const image = checkImage();
if (mode === '--check') {
  console.log('check only: no serial port opened, nothing written');
  process.exit(0);
}

const terminal = { clean() {}, writeLine: (line) => console.log(line), write: (text) => process.stdout.write(text) };
console.log(`waiting for the clock to power up in flashing mode on ${port}`);
await waitForChip(port, 15 * 60 * 1000);

const transport = new Transport(new NodeSerialDevice(port), false);
const loader = new ESPLoader({ transport, baudrate: 115200, romBaudrate: 115200, terminal });
console.log(`chip: ${await loader.main('no_reset')}`);
// writeFlash checks the chip's MD5 of what landed and throws on a mismatch.
await loader.writeFlash({
  fileArray: [{ data: image, address: 0 }],
  flashMode: 'keep',
  flashFreq: 'keep',
  flashSize: 'keep',
  eraseAll: false,
  compress: true,
  calculateMD5Hash: md5,
});
const size = image.length;
const chipMd5 = (await loader.flashMd5sum(0, size)).toLowerCase();
await transport.disconnect();
if (chipMd5 !== md5(image)) {
  console.error(`VERIFY FAILED: chip ${chipMd5}, file ${md5(image)}`);
  process.exit(1);
}
console.log(`VERIFIED: chip md5 ${chipMd5}. Unplug the clock and plug it back in with GPIO0 free.`);
process.exit(0);
