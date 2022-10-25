#include "recplaymgr.h"
#include "display.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// SD card
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <string.h>

#include "esp_log.h"

#include "lvgl.h"

#define SD_MISO GPIO_NUM_19
#define SD_MOSI GPIO_NUM_23
#define SD_CLK GPIO_NUM_18
#define SD_CS GPIO_NUM_5

#define TASK_STACK 4096

#define MOUNT_POINT "/sdcard"

sdmmc_card_t *mountSD(sdmmc_host_t *host) {
    esp_err_t ret;
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;

    ESP_LOGI("sdcard", "Initializing SD card");
    

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    ret = spi_bus_initialize(host->slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE("sdcard", "Failed to initialize bus.");
        return NULL;
    }
    
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = host->slot;

    ESP_LOGI("sdcard", "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE("sdcard", "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE("sdcard", "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return NULL;
    }
    ESP_LOGI("sdcard", "Filesystem mounted");

    sdmmc_card_print_info(stdout, card);
    
    return card;
}

void unmountSD(sdmmc_card_t *card, sdmmc_host_t *host) {
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_LOGI("sdcard", "Card unmounted");
    spi_bus_free(host->slot);
}

void example_lvgl_demo_ui(lv_disp_t *disp)
{
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    lv_label_set_text(label, "Hello Espressif, jhsakdlfjhas;ldkfjasddfjklklhsadl;kf");
    lv_obj_set_width(label, 128);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
}



void app_main() {

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdmmc_card_t *card = mountSD(&host);
    
    if (card == NULL) {
        ESP_LOGE("main", "SD card not detected");
        vTaskDelay(portMAX_DELAY);
    }
    
    
    lv_disp_t *disp = getDisplay();

    example_lvgl_demo_ui(disp);
    
    
    recPlayMgrInit();
    
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    
    startRec("/sdcard/1.wav");
    vTaskDelay(4000 / portTICK_PERIOD_MS);
    
    stopRec();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    startPlay("/sdcard/1.wav");
    vTaskDelay(2500 / portTICK_PERIOD_MS);
    
    // Stop everything
    stopPlay();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    unmountSD(card, &host);
}
