#include "sd_card.h"

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_console.h"

static const char *TAG = "SD_CARD";

static sdmmc_card_t *s_card = NULL;
static bool s_is_mounted = false;

esp_err_t sd_card_init(void)
{
    if (s_is_mounted) {
        ESP_LOGW(TAG, "SD card already mounted at %s", SD_CARD_MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing 1-bit SDMMC peripheral (CLK: GPIO14, CMD: GPIO15, DAT0: GPIO2)...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    /* Initialize SDMMC host in 1-bit mode */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; /* 20 MHz default for stability */

    /* Configure Slot 1 for 1-bit width */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem on SD card. Is card formatted as FAT32?");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card (error: %s). Check card insertion and wiring.", esp_err_to_name(ret));
        }
        s_card = NULL;
        s_is_mounted = false;
        return ret;
    }

    s_is_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully at %s", SD_CARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);

    return ESP_OK;
}

esp_err_t sd_card_deinit(void)
{
    if (!s_is_mounted) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_is_mounted = false;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card unmounted successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool sd_card_is_mounted(void)
{
    return s_is_mounted;
}

const sdmmc_card_t *sd_card_get_info(void)
{
    return s_card;
}

/* --- CONSOLE COMMANDS --- */

static int cmd_mount(int argc, char **argv)
{
    if (s_is_mounted) {
        printf("SD card is already mounted at %s.\n", SD_CARD_MOUNT_POINT);
        return 0;
    }
    printf("Attempting to mount SD card at %s...\n", SD_CARD_MOUNT_POINT);
    esp_err_t ret = sd_card_init();
    if (ret == ESP_OK) {
        printf("SD card mounted successfully!\n");
        return 0;
    } else {
        printf("Mount failed: %s (0x%x)\n", esp_err_to_name(ret), ret);
        return 1;
    }
}

static int cmd_unmount(int argc, char **argv)
{
    if (!s_is_mounted) {
        printf("SD card is not mounted.\n");
        return 0;
    }
    printf("Unmounting SD card...\n");
    esp_err_t ret = sd_card_deinit();
    if (ret == ESP_OK) {
        printf("SD card unmounted successfully.\n");
        return 0;
    } else {
        printf("Unmount failed: %s (0x%x)\n", esp_err_to_name(ret), ret);
        return 1;
    }
}

static int cmd_ls(int argc, char **argv)
{
    if (!s_is_mounted) {
        printf("SD card not mounted. Attempting auto-mount...\n");
        if (sd_card_init() != ESP_OK) {
            printf("Error: SD card auto-mount failed.\n");
            return 1;
        }
    }

    const char *target_dir = SD_CARD_MOUNT_POINT;
    char path_buf[300];

    if (argc >= 2) {
        if (argv[1][0] == '/') {
            snprintf(path_buf, sizeof(path_buf), "%s", argv[1]);
        } else {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", SD_CARD_MOUNT_POINT, argv[1]);
        }
        target_dir = path_buf;
    }

    DIR *dir = opendir(target_dir);
    if (!dir) {
        printf("Error: Unable to open directory '%s'\n", target_dir);
        return 1;
    }

    printf("Directory listing of: %s\n", target_dir);
    printf("%-8s  %-10s  %s\n", "Type", "Size (B)", "Name");
    printf("--------------------------------------------------\n");

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        char full_path[600];
        snprintf(full_path, sizeof(full_path), "%s/%s", target_dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            const char *type_str = S_ISDIR(st.st_mode) ? "[DIR]" : "[FILE]";
            if (S_ISDIR(st.st_mode)) {
                printf("%-8s  %-10s  %s\n", type_str, "-", entry->d_name);
            } else {
                printf("%-8s  %-10ld  %s\n", type_str, (long)st.st_size, entry->d_name);
            }
        } else {
            printf("%-8s  %-10s  %s\n", "[?]", "-", entry->d_name);
        }
        count++;
    }

    closedir(dir);
    printf("--------------------------------------------------\n");
    printf("Total entries: %d\n", count);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (!s_is_mounted) {
        printf("Error: SD card is not mounted.\n");
        return 1;
    }

    if (argc < 2) {
        printf("Usage: cat <filename> [max_bytes]\n");
        return 1;
    }

    char full_path[600];
    if (argv[1][0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", argv[1]);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", SD_CARD_MOUNT_POINT, argv[1]);
    }

    int max_bytes = 256;
    if (argc >= 3) {
        max_bytes = atoi(argv[2]);
        if (max_bytes <= 0 || max_bytes > 4096) {
            max_bytes = 256;
        }
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        printf("Error: Cannot open file '%s'\n", full_path);
        return 1;
    }

    char buf[128];
    int remaining = max_bytes;
    printf("--- Dumping first %d bytes of %s ---\n", max_bytes, full_path);

    while (remaining > 0) {
        size_t to_read = (remaining > (int)sizeof(buf)) ? sizeof(buf) : (size_t)remaining;
        size_t read_bytes = fread(buf, 1, to_read, f);
        if (read_bytes == 0) {
            break;
        }
        for (size_t i = 0; i < read_bytes; i++) {
            char c = buf[i];
            if (c >= 32 && c <= 126) {
                putchar(c);
            } else if (c == '\n' || c == '\r' || c == '\t') {
                putchar(c);
            } else {
                putchar('.');
            }
        }
        remaining -= (int)read_bytes;
    }
    putchar('\n');
    printf("--- End of dump ---\n");

    fclose(f);
    return 0;
}

static int cmd_sdinfo(int argc, char **argv)
{
    if (!s_is_mounted) {
        printf("SD card not mounted. Attempting auto-mount...\n");
        if (sd_card_init() != ESP_OK) {
            printf("Error: SD card auto-mount failed.\n");
            return 1;
        }
    }

    if (!s_card) {
        printf("SD card structure not available.\n");
        return 1;
    }

    printf("--- SD Card Information ---\n");
    sdmmc_card_print_info(stdout, s_card);
    printf("---------------------------\n");
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    printf("--- Memory Allocation & Heap Diagnostics ---\n");
    printf("Total free heap:         %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("Minimum free heap:       %lu bytes\n", (unsigned long)esp_get_minimum_free_heap_size());

    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    printf("Internal SRAM free:      %lu bytes (largest block: %lu bytes)\n",
           (unsigned long)internal_free, (unsigned long)internal_largest);

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    printf("External PSRAM free:     %lu bytes (largest block: %lu bytes)\n",
           (unsigned long)psram_free, (unsigned long)psram_largest);

    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    printf("DMA-capable memory free: %lu bytes (largest block: %lu bytes)\n",
           (unsigned long)dma_free, (unsigned long)dma_largest);

    printf("--------------------------------------------\n");
    return 0;
}

void sd_card_register_console_commands(void)
{
    esp_console_cmd_t mount_cmd = {
        .command = "mount",
        .help = "Mount the MicroSD card (SDMMC 1-bit)",
        .hint = NULL,
        .func = &cmd_mount,
    };
    esp_console_cmd_register(&mount_cmd);

    esp_console_cmd_t unmount_cmd = {
        .command = "unmount",
        .help = "Safely unmount the MicroSD card",
        .hint = NULL,
        .func = &cmd_unmount,
    };
    esp_console_cmd_register(&unmount_cmd);

    esp_console_cmd_t ls_cmd = {
        .command = "ls",
        .help = "List files and directories on SD card (e.g. ls or ls /sdcard/music)",
        .hint = "[path]",
        .func = &cmd_ls,
    };
    esp_console_cmd_register(&ls_cmd);

    esp_console_cmd_t cat_cmd = {
        .command = "cat",
        .help = "Dump file contents (e.g. cat song.wav 128)",
        .hint = "<filename> [max_bytes]",
        .func = &cmd_cat,
    };
    esp_console_cmd_register(&cat_cmd);

    esp_console_cmd_t sdinfo_cmd = {
        .command = "sdinfo",
        .help = "Display SD card technical details and capacity",
        .hint = NULL,
        .func = &cmd_sdinfo,
    };
    esp_console_cmd_register(&sdinfo_cmd);

    esp_console_cmd_t mem_cmd = {
        .command = "mem",
        .help = "Display internal SRAM, external PSRAM, and DMA heap statistics",
        .hint = NULL,
        .func = &cmd_mem,
    };
    esp_console_cmd_register(&mem_cmd);
}
