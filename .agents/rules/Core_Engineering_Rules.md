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

## 5. Living Documentation Maintenance
- `HARDWARE.md` and `README.md` must be kept accurate and up to date.
- Whenever hardware pinouts, board models, memory configurations, or software architecture changes occur, update these documents in the same step.
