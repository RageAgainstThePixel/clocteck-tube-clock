import type { ClockConfig, ClocTeckClient } from './client.js';
import type { Hsv } from './color.js';

export interface FlashOptions {
  color: Hsv;
  durationMs: number;
  /** Pulse brightness instead of holding a solid color */
  blink?: boolean;
}

export interface FlashResult {
  restoreAt: Date;
  /** True when this flash replaced one that was still showing */
  replaced: boolean;
}

const BLINK_INTERVAL_MS = 500;
const BLINK_DIM = 5;
const RESTORE_ATTEMPTS = 3;

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * Temporarily recolors the clock, then puts back whatever it showed before.
 *
 * The clock's settings are snapshotted when a flash starts from idle. A flash that arrives while
 * another is showing takes over its timer, so the clock always returns to the user's own settings,
 * never to another flash's color. Clock requests are serialized because the firmware handles one
 * connection at a time.
 */
export class AttentionController {
  private baseline: ClockConfig | undefined;
  private restoreTimer: NodeJS.Timeout | undefined;
  private blinkTimer: NodeJS.Timeout | undefined;
  private chain: Promise<unknown> = Promise.resolve();

  constructor(
    private readonly clock: ClocTeckClient,
    private readonly log: (message: string) => void = (m) => console.error(m),
  ) {}

  get active(): boolean {
    return this.baseline !== undefined;
  }

  /** Runs clock operations one after another, even across concurrent tool calls. */
  private serial<T>(fn: () => Promise<T>): Promise<T> {
    const next = this.chain.then(fn, fn);
    this.chain = next.catch(() => undefined);
    return next;
  }

  flash({ color, durationMs, blink = false }: FlashOptions): Promise<FlashResult> {
    return this.serial(async () => {
      const replaced = this.active;
      this.stopTimers();
      if (!this.baseline) this.baseline = await this.clock.getConfig();
      if (this.baseline.mode !== 'custom') await this.clock.setMode('custom', this.baseline.digitEffect);
      await this.clock.setAllTubes(color);
      if (blink) this.startBlink(color);
      this.restoreTimer = setTimeout(() => void this.restore(), durationMs);
      return { restoreAt: new Date(Date.now() + durationMs), replaced };
    });
  }

  private startBlink(color: Hsv) {
    let dim = false;
    this.blinkTimer = setInterval(() => {
      dim = !dim;
      // Brightness is global on this clock, so one request dims every tube
      void this.serial(() => this.clock.setTubeColor(1, { ...color, v: dim ? BLINK_DIM : color.v })).catch(() => undefined);
    }, BLINK_INTERVAL_MS);
  }

  private stopTimers() {
    clearTimeout(this.restoreTimer);
    clearInterval(this.blinkTimer);
    this.restoreTimer = this.blinkTimer = undefined;
  }

  /** Puts the snapshotted settings back. Safe to call when nothing is showing. */
  restore(): Promise<void> {
    return this.serial(async () => {
      this.stopTimers();
      const baseline = this.baseline;
      if (!baseline) return;
      for (let attempt = 1; ; attempt++) {
        try {
          for (const tube of baseline.tubes) {
            await this.clock.setTubeColor(tube.tube, { h: tube.hue, s: tube.saturation, v: baseline.brightness });
          }
          if (baseline.mode !== 'custom') await this.clock.setMode(baseline.mode, baseline.digitEffect);
          this.baseline = undefined;
          return;
        } catch (err) {
          const message = err instanceof Error ? err.message : String(err);
          if (attempt >= RESTORE_ATTEMPTS) {
            // Keep the baseline so the next flash still restores to the user's settings
            this.log(`Restoring the clock failed after ${attempt} attempts: ${message}`);
            return;
          }
          this.log(`Restoring the clock failed (attempt ${attempt}): ${message}; retrying`);
          await sleep(2000);
        }
      }
    });
  }
}
