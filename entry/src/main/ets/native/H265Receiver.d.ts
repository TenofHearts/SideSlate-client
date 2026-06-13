declare module 'libh265receiver.so' {
  export interface H265Stats {
    running: boolean;
    decoderStarted: boolean;
    surfaceReady: boolean;
    packets: number;
    bytes: number;
    queuedInputs: number;
    renderedOutputs: number;
    droppedPackets: number;
    sequenceGaps: number;
    configPackets: number;
    keyframes: number;
    lastSequence: number;
    queueDepth: number;
    streamWidth: number;
    streamHeight: number;
    streamFps: number;
    lastError: number;
    status: string;
  }

  const h265receiver: {
    start(port: number, width: number, height: number): boolean;
    stop(): void;
    getStats(): H265Stats;
  };

  export default h265receiver;
}
