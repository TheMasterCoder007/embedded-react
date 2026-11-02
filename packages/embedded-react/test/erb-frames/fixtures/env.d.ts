// What the build and the host give an app, as the starter template declares them.
declare const screen: {readonly width: number; readonly height: number};

declare module '*.png' {
  const src: string;
  export default src;
}

declare module '*.svg' {
  import type {SvgSource} from 'embedded-react';
  const src: SvgSource;
  export default src;
}
