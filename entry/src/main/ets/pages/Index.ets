import h265receiver, { H265Stats } from 'libh265receiver.so';
import { promptAction, window } from '@kit.ArkUI';

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
  @State private receiveMbps: number = 0;
  @State private inputRate: number = 0;
  @State private renderRate: number = 0;
  @State private dropRate: number = 0;
  @State private viewportWidth: number = 0;
  @State private viewportHeight: number = 0;
  @State private displayWidth: number = 0;
  @State private displayHeight: number = 0;
  @State private themeMode: string = 'system';

  private statsTimer: number = -1;
  private noStreamTimer: number = -1;
  private lastStatsAt: number = 0;
  private lastBytes: number = 0;
  private lastQueuedInputs: number = 0;
  private lastRendered: number = 0;
  private lastDropped: number = 0;
  private keepScreenOn: boolean = false;

  aboutToDisappear(): void {
    this.stopNoStreamTimer();
    this.stopReceiver();
    this.setKeepScreenOn(false);
  }

  onBackPress(): boolean {
    if (!this.inDisplayMode()) {
      return false;
    }

    this.stopReceiver();
    return true;
  }

  private inDisplayMode(): boolean {
    return this.running && this.decoderStarted && this.lastError === 0;
  }

  private connectionLabel(): string {
    if (this.lastError !== 0) {
      return 'Recovery';
    }
    if (this.inDisplayMode()) {
      return 'Streaming';
    }
    if (this.running) {
      return 'Connecting';
    }
    return 'Idle';
  }

  private connectionColor(): ResourceColor {
    if (this.lastError !== 0) {
      return 0xffb91c1c;
    }
    if (this.inDisplayMode()) {
      return 0xff15803d;
    }
    if (this.running) {
      return 0xff2563eb;
    }
    return 0xff334155;
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

  private startNoStreamTimer(): void {
    this.stopNoStreamTimer();
    this.noStreamTimer = setTimeout(() => {
      if (this.running && this.packets === 0) {
        this.stopReceiver();
        promptAction.showToast({
          message: 'No stream received from host within 10 seconds. Receiver stopped.',
          duration: 3500,
        });
      }
      this.noStreamTimer = -1;
    }, 10000);
  }

  private stopNoStreamTimer(): void {
    if (this.noStreamTimer !== -1) {
      clearTimeout(this.noStreamTimer);
      this.noStreamTimer = -1;
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
    if (stats.renderedOutputs > 0) {
      this.stopNoStreamTimer();
    }
  }

  private startReceiver(): void {
    this.lastStatsAt = 0;
    this.lastBytes = 0;
    this.lastQueuedInputs = 0;
    this.lastRendered = 0;
    this.lastDropped = 0;
    this.packets = 0;
    this.rendered = 0;
    this.dropped = 0;
    this.bytes = 0;
    this.queuedInputs = 0;
    this.queueDepth = 0;
    const started: boolean = h265receiver.start(LISTEN_PORT, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    this.running = started;
    this.status = started ? 'Receiver start requested.' : 'Receiver failed to start.';
    this.startStatsTimer();
    this.refreshStats();
    if (started) {
      this.startNoStreamTimer();
    }
  }

  private stopReceiver(): void {
    this.stopNoStreamTimer();
    h265receiver.stop();
    this.stopStatsTimer();
    this.refreshStats();
    this.status = 'Receiver stopped.';
    this.setKeepScreenOn(false);
  }

  private isDarkTheme(): boolean {
    return this.themeMode !== 'light';
  }

  private pageBackground(): ResourceColor {
    if (this.themeMode === 'system') return $r('app.color.t2s_page_background');
    return this.isDarkTheme() ? 0xff0b1220 : 0xffeef4f8;
  }

  private cardBackground(): ResourceColor {
    if (this.themeMode === 'system') return $r('app.color.t2s_card_background');
    return this.isDarkTheme() ? 0xee111827 : 0xeeffffff;
  }

  private secondaryBackground(): ResourceColor {
    if (this.themeMode === 'system') return $r('app.color.t2s_secondary_background');
    return this.isDarkTheme() ? 0xff1f2937 : 0xfff1f5f9;
  }

  private primaryText(): ResourceColor {
    if (this.themeMode === 'system') return $r('app.color.t2s_primary_text');
    return this.isDarkTheme() ? 0xfff8fafc : 0xff0f172a;
  }

  private secondaryText(): ResourceColor {
    if (this.themeMode === 'system') return $r('app.color.t2s_secondary_text');
    return this.isDarkTheme() ? 0xffcbd5e1 : 0xff475569;
  }

  private subtleText(): ResourceColor {
    if (this.themeMode === 'system') return $r('app.color.t2s_subtle_text');
    return this.isDarkTheme() ? 0xff94a3b8 : 0xff64748b;
  }

  private themeBorderColor(): ResourceColor {
    if (this.themeMode === 'system') return $r('app.color.t2s_border');
    return this.isDarkTheme() ? 0x26ffffff : 0x220f172a;
  }

  @Builder
  private AmbientBackground() {
    Column()
    .width('100%')
    .height('100%')
    .backgroundColor(this.pageBackground())
  }

  @Builder
  private ThemeChoice(label: string, value: string) {
    Button(label)
      .height(36)
      .padding({ left: 14, right: 14 })
      .backgroundColor(this.themeMode === value ? 0xff2563eb : this.secondaryBackground())
      .fontColor(this.themeMode === value ? 0xffffffff : this.secondaryText())
      .onClick(() => {
        this.themeMode = value;
      })
  }

  @Builder
  private StatPill(label: string, value: string, accent: ResourceColor) {
    Column({ space: 4 }) {
      Text(value)
        .fontSize(18)
        .fontWeight(FontWeight.Bold)
        .fontColor(accent)
      Text(label)
        .fontSize(11)
        .fontColor(this.subtleText())
    }
    .alignItems(HorizontalAlign.Start)
    .padding(12)
    .borderRadius(16)
    .backgroundColor(this.secondaryBackground())
    .border({ width: 1, color: this.themeBorderColor() })
    .layoutWeight(1)
  }

  @Builder
  private ConnectionMode() {
    Stack() {
      this.AmbientBackground()

      Column({ space: 16 }) {
        this.ReceiverCard()
        this.DiagnosticsCard()
        this.SettingsCard()
      }
      .width('100%')
      .height('100%')
      .padding(22)
    }
    .width('100%')
    .height('100%')
  }

  @Builder
  private ReceiverCard() {
    Column({ space: 14 }) {
      Row() {
        Text('RECEIVER')
          .fontSize(22)
          .fontWeight(FontWeight.Bold)
          .fontColor(this.primaryText())
          .layoutWeight(1)
          .textAlign(TextAlign.Start)
        Button(this.running ? 'Receiver Active' : 'Start Receiver')
          .enabled(!this.running)
          .height(44)
          .backgroundColor(0xff2563eb)
          .fontColor(0xffffffff)
          .onClick(() => this.startReceiver())
      }

      Row({ space: 12 }) {
        this.StatPill('USB transport', 'HDC forward', 0xff2563eb)
        this.StatPill('Listen port', `${LISTEN_PORT}`, 0xff2563eb)
        this.StatPill('Surface', this.surfaceReady ? 'Ready' : 'Waiting', this.surfaceReady ? 0xff16a34a : this.secondaryText())
        this.StatPill('Decoder', this.decoderStarted ? 'Started' : 'Idle', this.decoderStarted ? 0xff16a34a : this.secondaryText())
      }
      .width('100%')

      Text('Connect the tablet by USB, enable HDC forwarding on the host, then start the receiver. Display mode opens automatically when video frames arrive.')
        .fontSize(13)
        .fontColor(this.secondaryText())

      if (this.lastError !== 0) {
        Column({ space: 6 }) {
          Text('Recovery needed')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor(0xffb91c1c)
          Text(`Decoder error ${this.lastError}. Stop the receiver, verify the USB/HDC forward, then start again.`)
            .fontSize(13)
            .fontColor(0xffb91c1c)
        }
        .width('100%')
        .padding(12)
        .borderRadius(16)
        .backgroundColor(0x22ef4444)
      }
    }
    .width('100%')
    .padding(18)
    .borderRadius(24)
    .backgroundColor(this.cardBackground())
    .border({ width: 1, color: this.themeBorderColor() })
  }

  @Builder
  private DiagnosticsCard() {
    Column({ space: 14 }) {
      Text('DIAGNOSTICS')
        .fontSize(22)
        .fontWeight(FontWeight.Bold)
        .fontColor(this.primaryText())
        .width('100%')
        .textAlign(TextAlign.Start)

      Row({ space: 12 }) {
        this.StatPill('Input/s', this.inputRate.toFixed(1), this.primaryText())
        this.StatPill('Render/s', this.renderRate.toFixed(1), this.primaryText())
        this.StatPill('Drop/s', this.dropRate.toFixed(1), this.dropped > 0 ? 0xffdc2626 : this.primaryText())
        this.StatPill('Queue', `${this.queueDepth}`, this.primaryText())
      }
      .width('100%')

      Row({ space: 12 }) {
        this.StatPill('Packets', `${this.packets}`, this.primaryText())
        this.StatPill('Rendered', `${this.rendered}`, this.primaryText())
        this.StatPill('Dropped', `${this.dropped}`, this.dropped > 0 ? 0xffdc2626 : this.primaryText())
        this.StatPill('Bytes', `${this.bytes}`, this.primaryText())
      }
      .width('100%')

      Text(`Stream ${this.streamWidth} x ${this.streamHeight} @ ${this.streamFps} fps | Config ${this.configPackets} | Keyframes ${this.keyframes} | Gaps ${this.sequenceGaps}`)
        .fontSize(13)
        .fontColor(this.secondaryText())
    }
    .width('100%')
    .padding(18)
    .borderRadius(24)
    .backgroundColor(this.cardBackground())
    .border({ width: 1, color: this.themeBorderColor() })
  }

  @Builder
  private SettingsCard() {
    Column({ space: 14 }) {
      Text('SETTINGS')
        .fontSize(22)
        .fontWeight(FontWeight.Bold)
        .fontColor(this.primaryText())
        .width('100%')
        .textAlign(TextAlign.Start)

      Row() {
        Text('Appearance')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor(this.primaryText())
          .width(150)
        Text('Use system colors or force a light/dark receiver dashboard.')
          .fontSize(12)
          .fontColor(this.secondaryText())
          .layoutWeight(1)
        Row({ space: 8 }) {
          this.ThemeChoice('System', 'system')
          this.ThemeChoice('Light', 'light')
          this.ThemeChoice('Dark', 'dark')
        }
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      .padding(12)
      .borderRadius(16)
      .backgroundColor(this.secondaryBackground())
      .border({ width: 1, color: this.themeBorderColor() })
    }
    .width('100%')
    .padding(18)
    .borderRadius(24)
    .backgroundColor(this.cardBackground())
    .border({ width: 1, color: this.themeBorderColor() })
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      XComponent({ id: 'h265Surface', type: XComponentType.SURFACE, libraryname: 'h265receiver' })
        .width(this.displayWidth > 0 ? this.displayWidth : '100%')
        .height(this.displayHeight > 0 ? this.displayHeight : '100%')
        .backgroundColor(Color.Black)
        .opacity(this.inDisplayMode() ? 1 : 0)

      if (!this.inDisplayMode()) {
        this.ConnectionMode()
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
