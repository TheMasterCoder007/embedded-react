---
title: "Introduction"
description: "What Embedded React is and is not, the two flows, and the current status."
slug: /intro
---

**Embedded React is React Native for embedded MCUs.** You write a React app, build it, and flash it
onto a microcontroller. The UI runs *on the device*: no browser, no phone, and no operating system
required.

```jsx title="App.jsx"
import {View, Text, Pressable} from 'embedded-react';

export default function App() {
  return (
    <View style={{flex: 1, padding: 20, backgroundColor: '#1a1a2e'}}>
      <Text style={{color: '#fff', fontSize: 24}}>Hello from an ESP32.</Text>
      <Pressable onPress={() => console.log('tapped')}>
        <Text style={{color: '#e94560', marginTop: 12}}>Tap me</Text>
      </Pressable>
    </View>
  );
}
```

That component runs natively on a microcontroller driving a raw SPI or RGB display.

## What it is, and what it isn't

Most projects that pair "React" with an "ESP32" run React **in a web browser** on your phone or
laptop, talking to the microcontroller over REST or BLE. The MCU is only a backend; the component
tree, layout, and rendering all live somewhere else.

Embedded React is the opposite. It takes React Native's approach: React is a *component and
reconciliation model*, not a DOM thing. React Native swapped the browser's host primitives (div,
CSS, the browser layout engine) for native ones (View, Yoga, native draw calls). Embedded React
does that swap again, one level deeper. The host primitives are a **pure C99 engine drawing straight
into a framebuffer or SPI display**.

You write the same JSX components, the same Animated API, and the same flexbox styles you would
use on iOS or Android. Your app runs on an ESP32, STM32, or RP2040 instead of a phone.

## One app, two flows

The same JSX reaches the device through one of two flows. Both target the same C engine; the
difference is *when* the dynamism is resolved.

| | Flow A: runtime | Flow B: ahead of time |
|---|---|---|
| How it runs | A real React reconciler on [QuickJS](https://bellard.org/quickjs/), on the chip | JSX compiled to C and linked into the firmware |
| You get | Full runtime dynamism, hot reload on the device, UI updates without reflashing firmware | No JavaScript engine, no garbage collector, a smaller and deterministic binary |
| Needs | External RAM for the JavaScript heap: PSRAM on an ESP32-S3, SDRAM on an STM32H7 | Nothing extra: runs in internal RAM on MCUs with no external memory |
| Trade-off | RAM and per-frame dispatch cost | A [subset of the API](/guides/aot-subset) |

Choosing between them is a build flag, not a rewrite. [The two flows](/concepts/two-flows) goes
deeper.

## Status

Embedded React is in **beta**. The engine, both flows, the hardware backends below, and the
simulators are built and verified on real hardware; from here the work is fixes and features,
tracked in the [roadmap](/roadmap).

| Target | Flow | Status |
|---|---|---|
| ESP32-S3 with an 800×480 RGB panel | A | Verified on hardware |
| ESP32 "Cheap Yellow Display" (no PSRAM, SPI) | B | Verified on hardware |
| RP2040 with a 240×280 SPI display | B | Verified on hardware |
| Linux desktop (SDL) | A and B | Working |
| Browser (WebAssembly simulator) | A | Working |
| STM32H7 with SDRAM (Chrom-ART backend) | A | Running on hardware; a public example project is planned |
| Raspberry Pi | | Planned |

## Where to go next

- [Getting started](/getting-started): create a project and see it running in your browser in a
  couple of minutes, with no hardware.
- [Playground](/playground): try it without installing anything.
- [Concepts](/concepts): how the engine, the flows, and the rendering pipeline fit together.
