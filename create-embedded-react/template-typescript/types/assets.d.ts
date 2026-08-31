// Ambient declarations for asset imports. Importing an image/font yields an opaque handle the
// embedded-react engine bakes into the build. (Types for the `embedded-react` package itself ship with
// the package — no declaration needed here.)
declare module '*.png' {
  const src: string;
  export default src;
}
declare module '*.jpg' {
  const src: string;
  export default src;
}
declare module '*.jpeg' {
  const src: string;
  export default src;
}
declare module '*.webp' {
  const src: string;
  export default src;
}
// A `.svg` import is baked at build time into a vector op-tape (or a raster fallback when the SVG uses
// features the vector baker cannot represent) — an artifact object, not a path. Pass it to `<Svg source>`.
declare module '*.svg' {
  import type {SvgSource} from 'embedded-react';
  const src: SvgSource;
  export default src;
}
declare module '*.ttf' {
  const src: string;
  export default src;
}
declare module '*.otf' {
  const src: string;
  export default src;
}
