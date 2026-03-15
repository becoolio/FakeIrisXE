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
- Gen12 execlist and LRC groundwork
- firmware loading and GuC boot sequencing
- internal panel validation
- first real RCS command execution

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

This README reflects the current bring-up state based on the latest project tree, current boot artifacts, and the most recent RCS / execlist debugging work.

### Confirmed working

- Kext loads and matches the target iGPU
- `probe()` / `start()` path is active
- BAR0 / MMIO mapping is working
- FORCEWAKE acquisition is working
- GT forcewake now uses both **RENDER + GT** domains
- FORCEWAKE ACK now reaches `0x000000FF`
- Custom `IOFramebuffer` is created and published
- Internal display is brought online through the custom framebuffer path
- Current validated mode reaches **1920×1080**
- A custom `FakeIrisXEAccelerator` service is published
- Accelerator-related properties are exposed in IORegistry
- GEM allocation groundwork exists
- GGTT mapping groundwork exists
- DMC firmware loading succeeds in the observed boot path
- EXEClist submission path is now partially alive
- **Primary ELSP latch is confirmed working**
- BCS0 and VCS0 are observed as active engines with live state

### Present, but still partial

- IOSurface / shared-memory / fence plumbing
- User-client-side accelerator plumbing
- Gen12 execlist integration
- Logical Ring Context (LRC) construction
- GuC boot sequencing and state decoding
- Display identity / backlight / Apple-style presentation shims
- Engine recovery / reset handling
- Minimal batch submission path for proof-of-execution

### Not yet complete

- clean GuC authentication / firmware boot completion
- proven stable render command execution on RCS0
- proven stable execlist execution with valid Gen12 LRC
- validated real hardware acceleration for normal desktop use
- BLT submission completion
- command parser / batch submission completion
- multi-display support
- broader connector and mode validation
- hardened power management
- production stability

---

## Latest Technical Discoveries

The most recent bring-up work changed the project materially.

### Register and MMIO corrections

Several previously used register bases were wrong and have now been corrected:

- **RCS0 register base** corrected from `0x2C000` to `0x2000`
- **ELSP register addresses** corrected from `0x2C290 / 0x2C294` to `0x2290 / 0x2294`

These corrections align the current path more closely with Gen12/Linux i915 expectations and removed a major source of false-negative behavior during engine bring-up.

### GT / forcewake progress

- GT forcewake now uses both **RENDER + GT** domains
- Current observed forcewake ACK is `0x000000FF`

This was a necessary step for reliable register access during engine work, although it does **not** by itself prove that RCS0 is actually executing commands.

### EXEClist progress

A major recent milestone is that **primary ELSP submission now latches successfully**.

That means the project is no longer stuck at the earlier state where execlist submission was effectively dead on arrival. The submission port now appears to be alive enough to accept at least part of the intended context handoff.

This is a real breakthrough.

### Current RCS0 problem

RCS0 is still the main blocker.

Current observations:

- `GT_ERROR = 0x80054000`
- RCS validation reads back `START = 0` and `CTL = 0`
- RCS status is currently observed as **halted**
- current reported RCS state includes `STATUS = 0xE000`
- direct writes during validation are effectively ignored

In practice, this means:

- RCS0 is not behaving like a healthy running render engine
- legacy ring-style validation is not a reliable success test for Gen12
- the project needs a **proper Gen12 execlist + LRC path**, not more legacy-ring assumptions

### Other engine observations

Unlike RCS0, the following engines show signs of life:

- **BCS0** observed active with `MODE = 0x33`
- **VCS0** observed active with `MODE = 0x7`
- both show nonzero `HEAD / TAIL` activity

That matters because it suggests the GT is not uniformly dead. The render engine path is the broken part, not necessarily the entire GT fabric.

### Gen12 submission direction

The current evidence strongly points to this conclusion:

> **Gen12 requires proper execlist-based context initialization. Legacy ring submission logic is not enough.**

The next milestone is therefore not “more generic improvements,” but:

- build a valid **Gen12 RCS Logical Ring Context**
- build a valid **context descriptor**
- submit it through **ELSP**
- execute a **minimal proof-of-execution batch**
- confirm that GPU-visible memory is modified by the batch

Until that happens, the project should still be considered **display bring-up with partial engine progress**, not a working accelerated driver.

---

## Current Milestone

The current state can be summarized like this:

### Display path

- internal display online
- validated at **1920×1080**
- framebuffer publication working

### Submission path

- EXEClist path partially alive
- ELSP primary latch confirmed
- RCS0 still halted / wedged
- no confirmed real batch execution yet

### Firmware path

- DMC load path working
- GuC path no longer in the old dead-start condition
- GuC still **not** at a clean, final, known-good authenticated/runnable state

---

## Current Development Branch Notes

Recent build milestones have included:

- **V213** — EXEClist fallback for submission
- **V214** — imported multiple Linux i915-inspired GT improvements
- **V215** — GT recovery and engine fixes
- **V216** — clock gating register corrections
- **V217** — more aggressive power management experiments
- **V218** — expanded bring-up code with broader GT changes
- **V219** — attempted RCS active mode alignment
- **V220** — RCS unhalt + more aggressive EXEClist work
- **V221** — in progress, focused on more complete EXEClist integration

> [!IMPORTANT]
> The project has now moved past the stage where “add more random GT tweaks” is the right answer.
>
> The immediate priority is a **focused RCS0 Gen12 execlist/LRC implementation** that proves actual command execution.

---

## What Is Working Right Now

If described conservatively, the project currently provides:

- direct Tiger Lake PCI matching
- BAR0 / MMIO access
- forcewake handling
- custom framebuffer publication
- internal panel bring-up
- a working 1920×1080 display mode path
- custom accelerator service exposure
- partial memory-management scaffolding
- partial execlist submission progress
- live evidence that some GT engine paths are responding

That is real progress.

It is **not yet** the same as:

- stable render acceleration
- working Metal
- working OpenGL acceleration
- fully working command submission
- a finished Tiger Lake macOS graphics stack

---

## Immediate Focus

The project is currently focused on one goal:

> **Get RCS0 to execute a real minimal batch through a proper Gen12 LRC + EXEClist path.**

That means implementing and validating all of the following:

1. proper RCS0 state dump and diagnostics
2. safe RCS recovery / reset attempt path
3. allocation of execlist resources
4. construction of a valid Gen12 render LRC
5. construction of a valid context descriptor
6. ELSP submission of that context
7. polling for execlist progress
8. execution of a minimal proof batch such as:
   - `MI_NOOP`
   - `MI_STORE_DWORD_IMM`
   - `MI_BATCH_BUFFER_END`
9. confirmation that a scratch value in mapped memory actually changes

That writeback proof is the next real milestone.

Until then, ELSP latch alone is encouraging, but not enough.

---

## What Still Needs To Be Done

### Engine and submission work

- finish Gen12 RCS LRC builder
- finish V221

