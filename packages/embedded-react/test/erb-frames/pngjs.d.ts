/** The part of pngjs the frame test uses; the package ships no types. */
declare module 'pngjs' {
  export class PNG {
    constructor(options: {width: number; height: number});
    width: number;
    height: number;
    data: Buffer;
    static sync: {
      read(bytes: Buffer): PNG;
      write(png: PNG): Buffer;
    };
  }
}
