# ESP-IDF Coding Guidelines

When developing C code within the ESP-IDF framework for this project, adhere strictly to the following standards:

## 1. Error Handling
- Always capture the `esp_err_t` return value from ESP-IDF API functions.
- Use `ESP_ERROR_CHECK(ret);` to assert success on critical initialization steps where failure should halt execution.
- Never silently drop or ignore errors. For non-fatal operations, log failures explicitly via `ESP_LOGE`.

## 2. Logging
- Never use standard `printf` or `puts` for system logs.
- Always use the ESP-IDF logging library: `#include "esp_log.h"`.
- Declare a static module tag at the top of each compilation unit: `static const char *TAG = "MODULE_NAME";`.
- Use the appropriate log levels: `ESP_LOGE` for errors, `ESP_LOGW` for warnings, `ESP_LOGI` for key operational milestones, and `ESP_LOGD` / `ESP_LOGV` for debugging traces.

## 3. Memory Management
- Check the target memory architecture before allocating buffers:
  - If PSRAM is disabled, all allocations reside in the ~520 KB internal SRAM.
  - Keep stack sizes disciplined; never allocate large arrays or buffers on the FreeRTOS task stack.
  - Use `heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)` when targeting internal SRAM explicitly.
  - When PSRAM is enabled, use `MALLOC_CAP_SPIRAM` for large framebuffers or decoding stream buffers.
- Always check the return pointer for `NULL` after every dynamic allocation.
- Free all allocated buffers when tearing down tasks or state machines.

## 4. FreeRTOS Tasks
- Prefix all FreeRTOS task functions with `task_` (e.g., `void task_audio_decode(void *arg)`).
- Ensure infinite task loops contain a blocking call (queue receive, semaphore take, event group wait) or `vTaskDelay` to avoid starvation of the watchdog and lower-priority tasks.
- Assign appropriate task priorities based on real-time deadlines (e.g., audio pipeline higher priority than UI rendering).
