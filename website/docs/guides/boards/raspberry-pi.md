---
title: 'Raspberry Pi'
description: 'Planned: a Raspberry Pi 4 or 5 sample through an OpenGL ES or /dev/fb0 backend.'
---

:::info[Planned]
There is no Raspberry Pi example yet, and the two backends it would use, `opengl` (KMS or X11) and
`framebuffer` (direct `/dev/fb0`), are README placeholders in the repository. This page exists so the
sidebar is honest about what is and is not there.
:::

## What works today

A Pi running a desktop is a Linux machine with SDL2, so the [Linux](./linux.md) host builds and runs
on it the same way it does on a laptop: the app in an SDL window, Flow A or Flow B, with the same
engine. That is a reasonable way to develop on a Pi; it is not yet the fullscreen, no-desktop target
the planned backends are for.

## What the example will add

- A `framebuffer` backend writing straight to `/dev/fb0`, for a Pi driving a panel with no window
  system.
- An `opengl` backend for GL ES 2.0, for KMS output on the Pi or any board with a GL context.
- The example project itself, wiring the Pi's touch input into the engine.

Progress is tracked in the [roadmap](../../roadmap.md).
