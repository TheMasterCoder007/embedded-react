// What the host and the test runner (er-bridge-quickjs-runtest) give a runtime test.
declare const screen: {readonly width: number; readonly height: number};

/** Injects a touch: phase 0 down / 1 move / 2 up / 3 cancel. */
declare function __touch(
  phase: number,
  x: number,
  y: number,
  finger?: number,
): void;
/** How many layout passes the engine has run. */
declare function __layoutPasses(): number;
/** The last commit's repainted region as [x, y, width, height]. */
declare function __dirtyRect(): [number, number, number, number];
/** The framebuffer word at (x, y) as 0xAARRGGBB, or 0 when off-screen. */
declare function __pixel(x: number, y: number): number;
declare function __setWallClock(milliseconds: number): void;
/** Gives `screen` a new size and tells the app, as er_runtime_set_screen_size() does. */
declare function __setScreen(width: number, height: number): void;

declare var __runtime_failed: number;

/** What a split round trip's app leaves for er-bridge-quickjs-splittest to read. */
declare var __probe: {build: string; n: number};
