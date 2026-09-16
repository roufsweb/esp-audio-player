---
name: embedded-project-workflow
description: Full-stack embedded engineering workflow for managing large ESP-IDF projects with step-by-step implementation, factual verification, zero hackjobs, and living documentation.
---

# Embedded Project Workflow Skill

This skill guides full-stack embedded development on the ESP Audio Player project. It enforces technical precision, disciplined execution, and factual verification.

## 1. Step-by-Step Task Breakdown
Before writing any code or modifying configurations, divide the problem into discrete phases:
1. Requirement & Constraint Analysis (identify target chip, available RAM/PSRAM, pins, and protocols).
2. Hardware & Pinout Verification (ensure no pin collision with strapping pins, flash/PSRAM buses, or internal pull-ups).
3. Driver & Component Isolation (write or adapt modular drivers under `components/`).
4. Integration & FreeRTOS Task Wiring (connect components via RTOS queues/event groups).
5. Build & Hardware Verification (compile, check RAM usage from map file, verify on hardware).
6. Documentation Update (update `HARDWARE.md` and `README.md`).

## 2. Fact Verification Protocol
- Never assume GPIO availability, chip revisions, peripheral constraints, or bus speeds without verification.
- Always cross-reference:
  - ESP-IDF component headers (`esp_a2dp_api.h`, `driver/sdmmc_host.h`, etc.).
  - ESP32 Technical Reference Manual and datasheet.
  - Physical board schematics (Ai-Thinker ESP32-CAM / HW-297 schematics).
- If any detail is unverified, untested, or missing from local evidence, STOP and ask the user directly before proceeding.

## 3. High-Quality Code Standards (No Hackjobs)
- Write production-grade embedded C.
- Reject quick fixes: avoid magic number delays (`vTaskDelay(100)` used as synchronization hacks), undocumented bitwise tricks without commentary, or dummy polling loops.
- Avoid unnecessary code: do not create unused wrappers, dead helper functions, or bloated abstractions. Keep code lean and purposeful.
- Memory awareness: profile internal SRAM vs PSRAM allocations accurately. Check every allocation for `NULL`.

## 4. Documentation & Design Integrity
- Use clean, human-written technical prose.
- Do not use emojis in code, commits, or documentation.
- Avoid AI buzzwords and promotional language.
- Every architectural choice, pin allocation, and buffer dimension must have an engineering rationale.
