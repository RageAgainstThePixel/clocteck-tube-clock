// Makes a Node serialport look like the Web Serial device esptool-js expects (FW-OTA-008).
// DTR and RTS stay off: this board has no auto-reset wiring, and the adapter's RTS pin is not connected.
import { SerialPort } from 'serialport';

export class NodeSerialDevice {
  constructor(path) {
    this.path = path;
    this.port = null;
    this.readable = null;
    this.writable = null;
  }

  getInfo() {
    return { usbVendorId: 0x10c4, usbProductId: 0xea60 };
  }

  async open({ baudRate }) {
    const port = new SerialPort({ path: this.path, baudRate, autoOpen: false, hupcl: false });
    await new Promise((resolve, reject) => port.open((err) => (err ? reject(err) : resolve())));
    await new Promise((resolve, reject) => port.set({ dtr: false, rts: false }, (err) => (err ? reject(err) : resolve())));
    this.port = port;
    this.readable = new ReadableStream({
      start(controller) {
        port.on('data', (chunk) => controller.enqueue(new Uint8Array(chunk)));
        port.on('close', () => {
          try {
            controller.close();
          } catch {
            // already cancelled
          }
        });
      },
      cancel() {
        port.removeAllListeners('data');
      },
    });
    this.writable = new WritableStream({
      write(chunk) {
        return new Promise((resolve, reject) => {
          port.write(Buffer.from(chunk), (err) => {
            if (err) reject(err);
            else port.drain((drainErr) => (drainErr ? reject(drainErr) : resolve()));
          });
        });
      },
    });
  }

  async setBaudRate(baudRate) {
    await new Promise((resolve, reject) => this.port.update({ baudRate }, (err) => (err ? reject(err) : resolve())));
  }

  async setSignals() {
    // Not wired on this board.
  }

  async close() {
    const port = this.port;
    this.port = null;
    this.readable = null;
    this.writable = null;
    if (port && port.isOpen) {
      await new Promise((resolve) => port.close(() => resolve()));
    }
  }
}

/**
 * Waits for the boot ROM to report flashing mode at its 74880 baud, so nothing is sent into a board with no power.
 * Stray bytes from plugging and unplugging are ignored, and a normal boot keeps waiting for the next power-up.
 */
export async function waitForChip(path, timeoutMs) {
  const port = new SerialPort({ path, baudRate: 74880, autoOpen: false, hupcl: false });
  await new Promise((resolve, reject) => port.open((err) => (err ? reject(err) : resolve())));
  await new Promise((resolve, reject) => port.set({ dtr: false, rts: false }, (err) => (err ? reject(err) : resolve())));
  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('timed out waiting for the chip to power up')), timeoutMs);
      let seen = '';
      port.on('data', (chunk) => {
        seen = (seen + chunk.toString('latin1')).slice(-200);
        const match = seen.match(/boot mode:\((\d),\d\)/);
        if (!match) return;
        if (match[1] === '1') {
          clearTimeout(timer);
          port.removeAllListeners('data');
          console.log(`boot ROM reports flashing mode: ${match[0]}`);
          resolve();
        } else {
          console.log(`normal boot seen (${match[0]}), GPIO0 was not grounded; waiting for the next power-up`);
          seen = '';
        }
      });
    });
    // Let the boot ROM finish its startup text, and give the person time to release GPIO0 before any traffic.
    await new Promise((resolve) => setTimeout(resolve, 8000));
  } finally {
    await new Promise((resolve) => port.close(() => resolve()));
  }
}
