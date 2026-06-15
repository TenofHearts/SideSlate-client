import h265receiver, { H265Stats } from 'libh265receiver.so';
import { window } from '@kit.ArkUI';

const LISTEN_PORT: number = 7005;
const DEFAULT_WIDTH: number = 1920;
const DEFAULT_HEIGHT: number = 1080;

@Entry
@Component
struct Index {
  @State private running: boolean = false;
  @State private status: string = 'Receiver stopped.';
  @State private packets: number = 0;
  @State private rendered: number = 0;
  @State private dropped: number = 0;
  @State private bytes: number = 0;
  @State private queuedInputs: number = 0;
  @State private sequenceGaps: number = 0;
  @State private configPackets: number = 0;
  @State private keyframes: number = 0;
  @State private queueDepth: number = 0;
  @State private streamWidth: number = 0;
  @State private streamHeight: number = 0;
  @State private streamFps: number = 0;
  @State private surfaceReady: boolean = false;
  @State private decoderStarted: boolean = false;
  @State private lastError: number = 0;
  @State private showOverlay: boolean = true;
  @State private receiveMbps: number = 0;
  @State private inputRate: number = 0;
  @State private renderRate: number = 0;
  @State private dropRate: number = 0;
  @State private viewportWidth: number = 0;
  @State private viewportHeight: number = 0;
  @State private displayWidth: number = 0;
  @State private displayHeight: number = 0;

  private statsTimer: number = -1;
  private lastStatsAt: number = 0;
  private lastBytes: number = 0;
  private lastQueuedInputs: number = 0;
  private lastRendered: number = 0;
  private lastDropped: number = 0;
  private keepScreenOn: boolean = false;

  aboutToDisappear(): void {
    this.stopReceiver();
    this.setKeepScreenOn(false);
  }

  private updateDisplaySize(): void {
    const sourceWidth: number = this.streamWidth > 0 ? this.streamWidth : DEFAULT_WIDTH;
    const sourceHeight: number = this.streamHeight > 0 ? this.streamHeight : DEFAULT_HEIGHT;
    const viewportPixelWidth: number = this.viewportWidth > 0 ? vp2px(this.viewportWidth) : sourceWidth;
    const viewportPixelHeight: number = this.viewportHeight > 0 ? vp2px(this.viewportHeight) : sourceHeight;
    let width: number = this.viewportWidth > 0 ? this.viewportWidth * sourceWidth / viewportPixelWidth : px2vp(sourceWidth);
    let height: number = this.viewportHeight > 0 ? this.viewportHeight * sourceHeight / viewportPixelHeight : px2vp(sourceHeight);

    if (this.viewportWidth > 0 && this.viewportHeight > 0) {
      const scale: number = Math.min(1, this.viewportWidth / width, this.viewportHeight / height);
      width *= scale;
      height *= scale;
    }

    this.displayWidth = Math.max(1, width);
    this.displayHeight = Math.max(1, height);
  }

  private setKeepScreenOn(enabled: boolean): void {
    if (this.keepScreenOn === enabled) {
      return;
    }

    this.keepScreenOn = enabled;
    window.getLastWindow(getContext(this), (err, mainWindow) => {
      if (err.code) {
        this.status = `Failed to update screen wake state: ${JSON.stringify(err)}`;
        this.keepScreenOn = !enabled;
        return;
      }
      mainWindow.setWindowKeepScreenOn(enabled, (setErr) => {
        if (setErr.code) {
          this.status = `Failed to update screen wake state: ${JSON.stringify(setErr)}`;
          this.keepScreenOn = !enabled;
        }
      });
    });
  }

  private startStatsTimer(): void {
    this.stopStatsTimer();
    this.statsTimer = setInterval(() => this.refreshStats(), 500);
  }

  private stopStatsTimer(): void {
    if (this.statsTimer !== -1) {
      clearInterval(this.statsTimer);
      this.statsTimer = -1;
    }
  }

  private refreshStats(): void {
    const stats: H265Stats = h265receiver.getStats();
    const now: number = Date.now();
    if (this.lastStatsAt > 0) {
      const elapsedSeconds: number = Math.max((now - this.lastStatsAt) / 1000, 0.001);
      this.receiveMbps = (stats.bytes - this.lastBytes) * 8 / elapsedSeconds / 1000000;
      this.inputRate = (stats.queuedInputs - this.lastQueuedInputs) / elapsedSeconds;
      this.renderRate = (stats.renderedOutputs - this.lastRendered) / elapsedSeconds;
      this.dropRate = (stats.droppedPackets - this.lastDropped) / elapsedSeconds;
    }
    this.lastStatsAt = now;
    this.lastBytes = stats.bytes;
    this.lastQueuedInputs = stats.queuedInputs;
    this.lastRendered = stats.renderedOutputs;
    this.lastDropped = stats.droppedPackets;

    this.running = stats.running;
    this.status = stats.status;
    this.packets = stats.packets;
    this.rendered = stats.renderedOutputs;
    this.dropped = stats.droppedPackets;
    this.bytes = stats.bytes;
    this.queuedInputs = stats.queuedInputs;
    this.sequenceGaps = stats.sequenceGaps;
    this.configPackets = stats.configPackets;
    this.keyframes = stats.keyframes;
    this.queueDepth = stats.queueDepth;
    this.streamWidth = stats.streamWidth;
    this.streamHeight = stats.streamHeight;
    this.streamFps = stats.streamFps;
    this.updateDisplaySize();
    this.surfaceReady = stats.surfaceReady;
    this.decoderStarted = stats.decoderStarted;
    this.lastError = stats.lastError;
    this.setKeepScreenOn(stats.running && stats.decoderStarted);
    if (stats.renderedOutputs > 0 && stats.lastError === 0) {
      this.showOverlay = false;
    }
  }

  private startReceiver(): void {
    this.lastStatsAt = 0;
    this.lastBytes = 0;
    this.lastQueuedInputs = 0;
    this.lastRendered = 0;
    this.lastDropped = 0;
    const started: boolean = h265receiver.start(LISTEN_PORT, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    this.running = started;
    this.status = started ? 'Receiver start requested.' : 'Receiver failed to start.';
    this.showOverlay = true;
    this.startStatsTimer();
    this.refreshStats();
  }

  private stopReceiver(): void {
    h265receiver.stop();
    this.stopStatsTimer();
    this.refreshStats();
    this.status = 'Receiver stopped.';
    this.showOverlay = true;
    this.setKeepScreenOn(false);
  }

  @Builder
  private Overlay() {
    Column({ space: 18 }) {
      Text('Tablet2Screen')
        .fontSize(30)
        .fontWeight(FontWeight.Bold)
        .fontColor(Color.White)
      Text(this.status)
        .fontSize(16)
        .fontColor(0xd1d5db)

      Column({ space: 6 }) {
        Text('HDC setup on Windows')
          .fontSize(15)
          .fontWeight(FontWeight.Medium)
          .fontColor(Color.White)
        Text('hdc fport tcp:17005 tcp:7005')
          .fontSize(13)
          .fontFamily('monospace')
          .fontColor(0x93c5fd)
        Text(`Listening: 127.0.0.1:${LISTEN_PORT}`)
          .fontSize(13)
          .fontColor(0xd1d5db)
      }
      .width('100%')
      .padding(14)
      .backgroundColor(0x1f2937)
      .borderRadius(14)

      Row({ space: 12 }) {
        Button('Start Receiver')
          .enabled(!this.running)
          .onClick(() => this.startReceiver())
        Button('Stop')
          .enabled(this.running)
          .backgroundColor(0xdc2626)
          .onClick(() => this.stopReceiver())
        Button('Hide Overlay')
          .onClick(() => {
            this.showOverlay = false;
          })
      }

      Column({ space: 6 }) {
        Text(`Surface: ${this.surfaceReady}  Decoder: ${this.decoderStarted}  Error: ${this.lastError}`)
          .fontSize(13)
          .fontColor(0xd1d5db)
        Text(`Stream: ${this.streamWidth}x${this.streamHeight} @ ${this.streamFps} fps  Queue: ${this.queueDepth}`)
          .fontSize(13)
          .fontColor(0xd1d5db)
        Text(`Packets: ${this.packets}  Rendered: ${this.rendered}  Dropped: ${this.dropped}`)
          .fontSize(13)
          .fontColor(0xd1d5db)
        Text(`Rates: RX ${this.receiveMbps.toFixed(1)} Mbps  In ${this.inputRate.toFixed(1)}/s  Render ${this.renderRate.toFixed(1)}/s  Drop ${this.dropRate.toFixed(1)}/s`)
          .fontSize(13)
          .fontColor(0xd1d5db)
        Text(`Config: ${this.configPackets}  Keyframes: ${this.keyframes}  Gaps: ${this.sequenceGaps}`)
          .fontSize(13)
          .fontColor(0xd1d5db)
        Text(`Inputs: ${this.queuedInputs}  Bytes: ${this.bytes}`)
          .fontSize(13)
          .fontColor(0xd1d5db)
      }
      .width('100%')
      .padding(14)
      .backgroundColor(0x111827)
      .borderRadius(14)
    }
    .width(460)
    .padding(22)
    .backgroundColor(0xee0f172a)
    .borderRadius(22)
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      XComponent({ id: 'h265Surface', type: XComponentType.SURFACE, libraryname: 'h265receiver' })
        .width(this.displayWidth > 0 ? this.displayWidth : '100%')
        .height(this.displayHeight > 0 ? this.displayHeight : '100%')
        .backgroundColor(Color.Black)
        .onClick(() => {
          this.showOverlay = !this.showOverlay;
        })

      if (this.showOverlay) {
        this.Overlay()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
    .onAreaChange((_oldArea, newArea) => {
      this.viewportWidth = Number.parseFloat(newArea.width.toString());
      this.viewportHeight = Number.parseFloat(newArea.height.toString());
      this.updateDisplaySize();
    })
  }
}
