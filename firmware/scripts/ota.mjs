// FW-OTA-006: uploads a firmware image over Wi-Fi with the espota protocol, without Python.
//   npm run fw:ota                      uploads firmware/build/device/tubeclock.ino.bin to CLOCTECK_HOST
//   npm run fw:ota -- <host> [image]    names the clock and the image
//   npm run fw:ota -- --check [...]     checks the image and host and sends nothing
//   npm run fw:ota -- --force [...]     sends an image not named tubeclock*
// Test images have no Wi-Fi, so sending one means opening the case: hence --force.
// The token comes from .env and is never printed.
import { createHash } from 'node:crypto';
import dgram from 'node:dgram';
import { lookup } from 'node:dns/promises';
import { readFileSync } from 'node:fs';
import net from 'node:net';
import { basename, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const OTA_PORT = 8266;
const COMMAND_FLASH = 0;
const COMMAND_AUTH = 200;
const CHUNK_BYTES = 1460;
const ANSWER_MS = 10_000;
const RESULT_MS = 60_000;
const REBOOT_MS = 90_000;
const POLL_MS = 1000;
const IMAGE_LIMIT = 481280; // FW-BUD-001
const FLASH_MODE_DOUT = 3; // FW-HW-009

const md5 = (data) => createHash('md5').update(data).digest('hex');
const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..', '..');

function readEnv(key) {
  // The last match wins, as in gen-secrets.sh, which built this value into the firmware.
  const lines = [...readFileSync(resolve(root, '.env'), 'utf8').matchAll(new RegExp(`^${key}=(.*)$`, 'gm'))];
  return lines.length ? lines.at(-1)[1].trim().replace(/^"(.*)"$/, '$1') : '';
}

function checkImage(path) {
  const image = readFileSync(path);
  if (image.length > IMAGE_LIMIT) throw new Error(`image is ${image.length} bytes, over the ${IMAGE_LIMIT} limit`);
  if (image[0] !== 0xe9) throw new Error('not an ESP image');
  if (image[2] !== FLASH_MODE_DOUT) throw new Error(`flash mode is ${image[2]}, the ESP8285 needs DOUT`);
  return image;
}

/** Sends one UDP message and resolves with the clock's answer. */
function ask(socket, address, message) {
  return new Promise((done, fail) => {
    const timer = setTimeout(() => {
      socket.removeAllListeners('message');
      fail(new Error(`no answer from ${address}:${OTA_PORT}`));
    }, ANSWER_MS);
    socket.once('message', (reply) => {
      clearTimeout(timer);
      done(reply.toString());
    });
    socket.send(message, OTA_PORT, address);
  });
}

async function invite(address, image, imageMd5, localPort, token, name) {
  const socket = dgram.createSocket('udp4');
  try {
    const answer = await ask(socket, address, `${COMMAND_FLASH} ${localPort} ${image.length} ${imageMd5}\n`);
    if (answer === 'OK') return;
    if (!answer.startsWith('AUTH')) throw new Error(`unexpected answer: ${answer}`);
    const nonce = answer.split(/\s+/)[1];
    const cnonce = md5(`${name}${image.length}${imageMd5}${address}`);
    const result = md5(`${md5(token)}:${nonce}:${cnonce}`);
    const verdict = await ask(socket, address, `${COMMAND_AUTH} ${cnonce} ${result}\n`);
    if (verdict !== 'OK') throw new Error('the clock refused the token: does .env match the firmware on it?');
  } finally {
    socket.close();
  }
}

/**
 * Sends the image to the clock once it connects back: one chunk, one answer, then the final OK. The image
 * holds the Wi-Fi password and token, so a connection from anywhere but the clock is dropped unread.
 */
function sendImage(server, image, clockAddress) {
  return new Promise((done, fail) => {
    const waitConnect = setTimeout(
      () => fail(new Error('the clock did not connect back: Windows Firewall may be blocking Node, or restart the clock and retry')),
      ANSWER_MS,
    );
    const onConnection = async (connection) => {
      if (connection.remoteAddress?.replace(/^::ffff:/, '') !== clockAddress) {
        connection.destroy();
        return;
      }
      server.off('connection', onConnection);
      clearTimeout(waitConnect);
      let replies = '';
      let wake = null;
      connection.on('data', (chunk) => {
        replies += chunk.toString();
        if (wake) wake();
      });
      connection.on('error', (err) => fail(err));
      const nextReply = (ms) =>
        new Promise((ready, timedOut) => {
          const timer = setTimeout(() => timedOut(new Error('the clock stopped answering')), ms);
          wake = () => {
            clearTimeout(timer);
            wake = null;
            ready();
          };
          if (replies) wake();
        });
      try {
        let lastPercent = -10;
        for (let offset = 0; offset < image.length; offset += CHUNK_BYTES) {
          connection.write(image.subarray(offset, offset + CHUNK_BYTES));
          await nextReply(ANSWER_MS);
          if (replies.includes('E')) throw new Error(`the clock reported: ${replies}`);
          if (offset + CHUNK_BYTES < image.length) replies = '';
          const percent = Math.floor(((offset + CHUNK_BYTES) / image.length) * 100);
          if (percent >= lastPercent + 10) {
            lastPercent = percent;
            console.log(`sent ${Math.min(percent, 100)}%`);
          }
        }
        // The clock checks the image's MD5 before it says OK.
        const deadline = Date.now() + RESULT_MS;
        while (!replies.includes('O') && !replies.includes('E')) {
          if (Date.now() > deadline) throw new Error('no result from the clock');
          replies = '';
          await nextReply(RESULT_MS);
        }
        if (replies.includes('E')) throw new Error(`the clock reported: ${replies}`);
        connection.end();
        done();
      } catch (err) {
        connection.destroy();
        fail(err);
      }
    };
    server.on('connection', onConnection);
  });
}

async function readState(host, token) {
  const response = await fetch(`http://${host}/api/state`, {
    headers: { Authorization: `Bearer ${token}` },
    signal: AbortSignal.timeout(4000),
  });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

async function main() {
  const args = process.argv.slice(2);
  const take = (flag) => {
    const at = args.indexOf(flag);
    if (at >= 0) args.splice(at, 1);
    return at >= 0;
  };
  const checkOnly = take('--check');
  const force = take('--force');
  const host = args[0] || readEnv('CLOCTECK_HOST');
  if (!host) throw new Error('name a host, or set CLOCTECK_HOST in .env');
  const path = resolve(args[1] || resolve(root, 'firmware/build/device/tubeclock.ino.bin'));
  const token = readEnv('FW_API_TOKEN');
  if (!token) throw new Error('FW_API_TOKEN is empty in .env');

  if (!basename(path).startsWith('tubeclock') && !force) {
    throw new Error(`${basename(path)} is not a tubeclock image, and a test image has no Wi-Fi: --force sends it anyway`);
  }
  const image = checkImage(path);
  const imageMd5 = md5(image);
  const { address } = await lookup(host, { family: 4 });
  console.log(`image ${basename(path)}, ${image.length} bytes, md5 ${imageMd5}`);
  console.log(`clock ${host} (${address})`);
  if (checkOnly) {
    console.log('check only: nothing sent');
    return;
  }

  const before = await readState(address, token).catch(() => null);
  if (before) console.log(`running ${before.version}, last synced ${before.lastSync}`);

  const server = net.createServer();
  await new Promise((ready, fail) => {
    server.once('error', fail);
    server.listen(0, '0.0.0.0', ready);
  });
  try {
    await invite(address, image, imageMd5, server.address().port, token, basename(path));
    // The clock connects back only after this, and no event can run between that answer and this listener.
    await sendImage(server, image, address);
  } finally {
    server.close();
  }
  console.log('upload accepted, waiting for the clock to restart');

  // A restart takes the clock off Wi-Fi for seconds, so an unanswered poll then an answer means it restarted.
  // Polling starts at once, before the clock can drop and return unseen.
  const deadline = Date.now() + REBOOT_MS;
  let wentAway = false;
  while (Date.now() < deadline) {
    const state = await readState(address, token).catch(() => null);
    if (!state) {
      wentAway = true;
    } else if (wentAway || (before && state.version !== before.version)) {
      console.log(`back up, running ${state.version}`);
      return;
    }
    await new Promise((wait) => setTimeout(wait, POLL_MS));
  }
  throw new Error(
    wentAway
      ? 'the upload was accepted, but the clock has not come back on Wi-Fi'
      : 'the upload was accepted, but the clock kept answering and never restarted',
  );
}

main().catch((err) => {
  console.error(`OTA failed: ${err.message}`);
  process.exit(1);
});
