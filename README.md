# FakeIrisXE

> Experimental standalone macOS graphics kext for **Intel Iris Xe / Tiger Lake** bring-up on unsupported systems.

[![Status](https://img.shields.io/badge/status-experimental-orange)](./)
[![Platform](https://img.shields.io/badge/platform-macOS-blue)](./)
[![Target](https://img.shields.io/badge/GPU-Intel%20Tiger%20Lake-6f42c1)](./)
[![Type](https://img.shields.io/badge/type-standalone%20kext-critical)](./)

---

## Overview

**FakeIrisXE** is a custom **standalone IOKit graphics driver** focused on bringing up **Intel Tiger Lake Iris Xe graphics** under macOS on systems that are not natively supported.

This project is **not** a Lilu plugin.

Instead, it matches the Intel iGPU directly as an `IOPCIDevice`, publishes its own custom `IOFramebuffer`, exposes a custom accelerator service, and incrementally implements the low-level pieces needed for display bring-up and eventual real GPU submission.

The current work is centered on:

- direct hardware bring-up
- MMIO / BAR0 access
- display pipeline initialization
- framebuffer publication
- accelerator stack scaffolding
- GEM / GGTT groundwork
- ring and execlist groundwork
- firmware loading and GuC boot sequencing
- internal panel validation

---

## Project Status

> [!WARNING]
> This repository is **highly experimental**. It is not ready for production, daily driving, or general-user deployment.

> [!CAUTION]
> Low-level graphics work can cause:
> - black screens
> - corrupted output
> - boot hangs
> - kernel panics
> - forced shutdowns
> - cache rebuilds
> - recovery-mode detours
> - broken installs
> - missing weekends

> [!IMPORTANT]
> Current progress is real, but this is still a **bring-up project**, not a finished graphics driver.

---

## Disclaimer

> [!IMPORTANT]
> **FakeIrisXE** is an experimental open-source research project for education, interoperability study, reverse-engineering practice, and hardware bring-up work.
>
> It is shared so others can learn from it, inspect it, test it, improve it, and build on it responsibly. It is **not** a commercial product, **not** a support contract, and **not** a promise that your machine will boot, render, wake, sleep, accelerate, or behave.

> [!WARNING]
> By using this project, you accept that low-level graphics development can cause:
> - kernel panics
> - black screens
> - corrupted output
> - broken installs
> - forced reboots
> - missing weekends
> - and sudden character development

> [!CAUTION]
> This software is provided **AS IS**, with **no warranty** and **no guarantee** of fitness, reliability, safety, stability, legality, or usefulness for any specific purpose.
>
> The authors and contributors are **not responsible** for:
> - your alarm clock app failing and you losing your job
> - your screen turning into 1997 cable TV static five minutes before an important meeting
> - your Hackintosh refusing to boot after “one small change”
> - lost data, lost time, lost sleep, lost patience, or lost weekends

> [!NOTE]
> This project is published in the spirit of open source:
> - to share knowledge
> - to document progress
> - to invite careful experimentation
> - to help others learn how this class of problem works
>
> That spirit does **not** mean anyone here owes you guaranteed fixes, custom support, production readiness, or free engineering labor on demand.

> [!TIP]
> Contributions, testing, logs, documentation improvements, and technically sound issue reports are welcome.
>
> Homework dumping is not.
>
> Please do not open issues or requests expecting the maintainers to:
> - do your class assignment for you
> - write your project report for you
> - debug your entire setup from a one-line message saying `it broke`
> - teach concepts you have not attempted to read about first
>
> Show your work, share your logs, explain what you tested, and meet the project halfway.

> [!IMPORTANT]
> **FakeIrisXE is not affiliated with, endorsed by, or sponsored by Apple.**

> [!IMPORTANT]
> This repository should not be used to distribute proprietary code, firmware, binaries, symbols, SDK content, trademarks, or other third-party material unless the contributor has the legal right to do so.

---

## Current Target Hardware

### PCI IDs currently matched

- `8086:9A49` — primary Tiger Lake target
- `8086:46A3` — secondary matched target currently using Tiger Lake fallback handling in-tree

### Current display focus

- built-in internal panel
- single-display bring-up path
- currently validated mode path: **1920 × 1080**

---

## Current State

This README reflects the current bring-up state based on the latest project tree and uploaded boot artifacts.

### Confirmed working

- Kext loads and matches the target iGPU
- `probe()` / `start()` path is active
- BAR0 / MMIO mapping is working
- FORCEWAKE acquisition is working
- GT early bring-up is active
- Custom `IOFramebuffer` is created and published
- Internal display is brought online through the custom framebuffer path
- Current validated mode reaches **1920×1080**
- Current observed stride is **7680 bytes**
- A custom `FakeIrisXEAccelerator` service is published
- Accelerator-related properties are exposed in IORegistry
- GEM allocation groundwork exists
- GGTT mapping groundwork exists
- RCS ring creation path exists
- BLT ring scaffolding exists
- DMC firmware loading succeeds in the current observed boot path
- GuC bring-up now progresses beyond the old dead-start state

### Present, but still partial

- IOSurface / shared-memory / fence plumbing
- Execlist-related submission scaffolding
- User-client-side accelerator plumbing
- Metal-facing and acceleration-facing property publication
- GuC boot progress past dead-start, but not to a clean final success state
- Display identity / backlight / Apple-style presentation shims

### Not yet complete

- clean GuC authentication / firmware boot completion
- stable command submission
- proven render execution
- proven stable execlist execution
- validated real hardware acceleration for normal desktop use
- BLT submission completion
- command parser / batch submission completion
- multi-display support
- broader connector and mode validation
- hardened power management
- production stability

---

## Latest Bring-Up Milestone

A major improvement in the current boot path is that the GuC flow is no longer stuck at the old dead-start condition:

```text
GUC_STATUS = 0x00000001
