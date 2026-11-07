// FW-OTA-008: reads the clock's whole flash to a new file and checks it against the chip's own MD5.
//   npm run fw:backup -- [port] [baud]     default COM4 at 115200
// Read-only: it loads esptool's helper stub into RAM and never calls a flash write or erase.
// The image holds the saved Wi-Fi password, so it goes in the git-ignored clock/backups/ and its contents are
// never printed. A new file every time, so a later backup can never replace the stock one.
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ESPLoader, Transport } from 'esptool-js';
import { NodeSerialDevice, waitForChip } from './serial-device.mjs';

const PORT = process.argv[2] || 'COM4';
const BAUD = Number(process.argv[3] || 115200);
const FLASH_BYTES = 0x100000;
const here = dirname(fileURLToPath(import.meta.url));
const outDir = resolve(here, '..', '..', 'clock', 'backups');

const terminal = {
  clean() {},
  writeLine: (line) => console.log(line),
  write: (text) => process.stdout.write(text),
};

console.log(`waiting for the clock to power up in flashing mode on ${PORT}`);
await waitForChip(PORT, 15 * 60 * 1000);

async function readAll() {
  const device = new NodeSerialDevice(PORT);
  const transport = new Transport(device, false);
  try {
    const loader = new ESPLoader({ transport, baudrate: BAUD, romBaudrate: 115200, terminal });
    const chip = await loader.main('no_reset');
    console.log(`chip: ${chip}`);
    const mac = (await loader.chip.readMac(loader)).replaceAll(':', '');

    let lastPercent = -10;
    const started = Date.now();
    const image = await loader.readFlash(0, FLASH_BYTES, (_packet, done, total) => {
      const percent = Math.floor((done / total) * 100);
      if (percent >= lastPercent + 10) {
        lastPercent = percent;
        console.log(`read ${percent}%`);
      }
    });
    console.log(`read ${image.length} bytes in ${Math.round((Date.now() - started) / 1000)} s`);
    const chipMd5 = (await loader.flashMd5sum(0, FLASH_BYTES)).toLowerCase();
    return { image, mac, chipMd5 };
  } finally {
    await transport.disconnect().catch(() => {});
  }
}

let result;
for (let attempt = 1; attempt <= 3 && !result; attempt++) {
  try {
    result = await readAll();
  } catch (err) {
    console.log(`attempt ${attempt} failed: ${err.message}`);
    await new Promise((done) => setTimeout(done, 1000));
  }
}
if (!result) {
  console.error('FAILED: no attempt finished, nothing saved');
  process.exit(1);
}

const { image, mac, chipMd5 } = result;
const localMd5 = createHash('md5').update(image).digest('hex');
console.log(`md5 from file: ${localMd5}`);
console.log(`md5 from chip: ${chipMd5}`);

if (image.length !== FLASH_BYTES || localMd5 !== chipMd5) {
  console.error('VERIFY FAILED: not saving this image');
  process.exit(1);
}

mkdirSync(outDir, { recursive: true });
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace('T', '-').slice(0, 15);
const name = `flash-${mac}-${stamp}.bin`;
const out = resolve(outDir, name);
if (existsSync(out)) {
  console.error(`${name} already exists: not replacing it`);
  process.exit(1);
}
writeFileSync(`${out}.tmp`, image);
renameSync(`${out}.tmp`, out);
const sha256 = createHash('sha256').update(readFileSync(out)).digest('hex');
writeFileSync(`${out}.sha256`, `${sha256}  ${name}\n`);
console.log(`VERIFIED and saved: ${out}`);
console.log(`sha256: ${sha256}`);
process.exit(0);
