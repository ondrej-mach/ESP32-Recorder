#include "recplaymgr.h"
#include "display.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "freertos/queue.h"

#include "driver/gpio.h"

// SD card
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

// ls snippet taken from https://stackoverflow.com/questions/13554150/implementing-the-ls-al-command-in-c
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

#include <string.h>

#include "esp_log.h"

#include "lvgl.h"

#define SD_MISO GPIO_NUM_19
#define SD_MOSI GPIO_NUM_23
#define SD_CLK GPIO_NUM_18
#define SD_CS GPIO_NUM_5

#define BUTTON_UP GPIO_NUM_13
#define BUTTON_DOWN GPIO_NUM_12
#define BUTTON_OK GPIO_NUM_14

#define TASK_STACK 4096

#define MOUNT_POINT "/sdcard"

TaskHandle_t UITaskHandle;
QueueHandle_t buttonQueue;
lv_disp_t *disp;

typedef enum { UP, DOWN, OK, OK_LONG } ButtonEvent;
int buttons[] = {BUTTON_UP, BUTTON_DOWN, BUTTON_OK};


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
    lv_label_set_text(label, "Hello Espressif\nXD");
    lv_obj_set_width(label, 128);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
}

void lvPrint(lv_disp_t *disp, char *text) {
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_clean(scr);
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 128);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
    //lv_obj_invalidate();
    lv_refr_now(disp);
}

int showFiles(lv_disp_t *disp, int selected) {
    const int LINES_FIT = 8;
    const int LINES_BEFORE = 2;
    
    DIR *mydir;
    struct dirent *myfile;
    char filename[32];

    char buf[512] = {0};
    mydir = opendir(MOUNT_POINT);
    
    int index = 0;
    while (true) {
        if (index == 0) {
            strcpy(filename, "[Record]");
        } else {
            myfile = readdir(mydir);
            if (myfile == NULL) {
                break;
            }
            strcpy(filename, myfile->d_name);
        }
            
        if ((index >= selected - LINES_BEFORE) && (index <= selected + LINES_FIT)) {
            strcat(buf,  (index == selected) ? "> " : "  ");
            strcat(buf, filename);
            strcat(buf, "\n");
        }
        index++;
    }
    closedir(mydir);
    lvPrint(disp, buf);
    return index;
}

void getFilenameFromIndex(char *filename, int selected) {
    DIR *mydir;
    struct dirent *myfile;

    mydir = opendir(MOUNT_POINT);
    int index = 1;
    while((myfile = readdir(mydir)) != NULL) {
        if (index == selected) {
            sprintf(filename, "%s/%s", MOUNT_POINT, myfile->d_name);
            break;
        }
        index++;
    }
    closedir(mydir);
}
    
void getNewFilename(char *filename) {
    int index = 1;
    do {
        sprintf(filename, "%s/%d.wav", MOUNT_POINT, index);
        index++;
    } while (access(filename, F_OK) == 0);
}




void gpioHandler(void* arg)
{
	printf("Interrupt\n");
}

// code from https://esp32tutorials.com/esp32-gpio-interrupts-esp-idf/
static void IRAM_ATTR buttonISR(void *args) {
    //uint32_t gpio_num = READ_PERI_REG(GPIO_STATUS_REG);

    int pin = (int)args;
    
    static int pinHolding = -1;
    static uint32_t pushTime = 0;
    
    bool relevant = false;
    for (int i=0; i<(sizeof(buttons)/sizeof(int)); i++) {
        if (buttons[i] == pin) {
            relevant = true;
        }
    }
    if (!relevant) {
        return;
    }
    
    if (gpio_get_level(pin)) {
        // new push
        if (pinHolding == -1) {
            pinHolding = pin;
            pushTime = esp_log_timestamp();
        }
    } else {
        // on release
        if (pinHolding == pin) {
            int holdTime = esp_log_timestamp() - pushTime;
            ButtonEvent e;
            switch (pin) {
                case BUTTON_UP:
                    e = UP;
                    break;
                    
                case BUTTON_DOWN:
                    e = DOWN;
                    break;
                    
                case BUTTON_OK:
                    e = (holdTime < 500) ? OK : OK_LONG;
                    break;
            }
            xQueueSendFromISR(buttonQueue, &e, 0);
            pinHolding = -1;
        }
    }
}

ButtonEvent waitEvent() {
    while (true) {
        for (int i=0; i<(sizeof(buttons)/sizeof(int)); i++) {
            gpio_set_direction(buttons[i], GPIO_MODE_INPUT);
            gpio_pulldown_en(buttons[i]);
            gpio_pullup_dis(buttons[i]);
            // gpio_set_intr_type(buttons[i], GPIO_INTR_ANYEDGE);
            // gpio_isr_handler_add(buttons[i], buttonISR, (void *)buttons[i]);
        }
        vTaskDelay(portMAX_DELAY);
    }
}

void UITask() {
    recPlayMgrInit();
    
    ButtonEvent e;
    int menuIndex = 0;
    int menuItemsCount = showFiles(disp, 0);
    char filename[32];
    
    while (xQueueReceive(buttonQueue, &e, portMAX_DELAY)) {
        printf("Event %d\n", e);
        switch (e) {
            case UP:
                menuIndex += menuItemsCount - 1;
                menuIndex %= menuItemsCount;
                break;
                
            case DOWN:
                menuIndex++;
                menuIndex %= menuItemsCount;
                break;
                
            case OK:
                if (menuIndex == 0) {
                    getNewFilename(filename);
                    startRec(filename);
                    lvPrint(disp, "RECORDING...");
                    xQueueReceive(buttonQueue, &e, portMAX_DELAY);
                } else {
                    getFilenameFromIndex(filename, menuIndex);
                    startPlay(filename);
                }
                break;
                
            case OK_LONG:
                break;
        }
        menuItemsCount = showFiles(disp, menuIndex);
    }
}



void app_main() {
    disp = getDisplay();
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdmmc_card_t *card = mountSD(&host);
    if (card == NULL) {
        lvPrint(disp, "NO SD CARD");
        ESP_LOGE("main", "SD card not detected");
        vTaskDelay(portMAX_DELAY);
    }

    buttonQueue = xQueueCreate(1, sizeof(ButtonEvent));
    
    for (int i=0; i<(sizeof(buttons)/sizeof(int)); i++) {
        // gpio_pad_select_gpio(buttons[i]);
        gpio_set_direction(buttons[i], GPIO_MODE_INPUT);
        gpio_pulldown_en(buttons[i]);
        gpio_pullup_dis(buttons[i]);
        gpio_set_intr_type(buttons[i], GPIO_INTR_ANYEDGE);
        gpio_intr_enable(buttons[i]);
        // gpio_set_intr_type(buttons[i], GPIO_INTR_ANYEDGE);
        // gpio_isr_handler_add(buttons[i], buttonISR, (void *)buttons[i]);
    }
    gpio_isr_register(gpioHandler, NULL, 0, NULL);
    
    
    xTaskCreate(UITask, "UI", 16384, NULL, 1, &UITaskHandle);
    
    vTaskDelay(portMAX_DELAY);
    
    ///////////////////////////////////// USELESS STUFF

    showFiles(disp, 0);

    char newFilename[32];
    getNewFilename(newFilename);
    
    recPlayMgrInit();
    
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    
    startRec(newFilename);
    vTaskDelay(4000 / portTICK_PERIOD_MS);
    
    stopRec();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    
    startPlay(newFilename);
    vTaskDelay(2500 / portTICK_PERIOD_MS);
    
    // Stop everything
    stopPlay();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    unmountSD(card, &host);
}
