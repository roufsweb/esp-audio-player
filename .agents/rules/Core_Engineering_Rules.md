# Core Engineering Rules

These rules govern all engineering, development, and documentation work on this project. Strictly adhere to them at all times.

## 1. Verified Facts Only
- Always provide answers, design recommendations, and code based on verified facts (ESP-IDF source code, official Espressif datasheets, hardware schematics, and actual measurements).
- If any detail regarding hardware connections, chip revisions, pin availability, or protocol behavior is unverified or ambiguous, STOP and ask the user directly before making assumptions or writing code.

## 2. Step-by-Step Implementation
- Always break down complex tasks, subsystems, and refactoring into discrete, verifiable phases.
- Do not implement multiple unrelated subsystems simultaneously.
- Verify each phase (compilation, memory layout, peripheral initialization) before proceeding to the next.

## 3. Engineering Rigor (No Hackjobs, No Dead Code)
- Act as a senior embedded systems engineer.
- Never use hacky workarounds, blind delays, or quick fixes that mask root causes.
- Do not write unnecessary code, bloatware, or premature abstractions. Keep the codebase clean, lean, and purpose-built.
- Respect physical memory limits (SRAM vs PSRAM) and CPU cycles.

## 4. Documentation Standards (No Emojis, No AI Fluff)
- All documentation, code comments, and commit messages must be technical, practical, and human-readable.
- Strictly forbidden: Emojis of any kind.
- Strictly forbidden: AI fluff, generic marketing buzzwords (e.g., "seamless", "cutting-edge", "delve", "leverage", "tapestry").
- Focus on real hardware facts: GPIO maps, timing requirements, memory footprints, buffer sizes, and API contracts.

## 5. Living Documentation & Feature Maintenance
- `README.md`, `FEATURES.md`, and `HARDWARE.md` must be kept accurate and up to date.
- Whenever any feature is added, modified, optimized, or verified, update `README.md` and `FEATURES.md` in the exact same step.
- Document all user-facing console commands, format capabilities, bitrates, buffer allocations, and architectural decisions.
- Maintain a clear status log of verified working features vs planned items.
