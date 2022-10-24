
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h> 

// SD card
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <string.h>

#include "esp_log.h"

#include "wav.h"

// display
// #include "esp_lcd_panel_io.h"
// #include "esp_lcd_panel_vendor.h"
// #include "esp_lcd_panel_ops.h"
// #include "driver/i2c.h"
// #include "esp_err.h"
// #include "esp_log.h"
// #include "lvgl.h"

#define MIC_DOUT GPIO_NUM_16
#define MIC_BCLK GPIO_NUM_17
#define MIC_WS GPIO_NUM_25

#define AMP_DIN GPIO_NUM_2
#define AMP_BCLK GPIO_NUM_4
#define AMP_WS GPIO_NUM_27

#define SD_MISO GPIO_NUM_19
#define SD_MOSI GPIO_NUM_23
#define SD_CLK GPIO_NUM_18
#define SD_CS GPIO_NUM_5

#define LED_PIN GPIO_NUM_26

#define SAMPLING_RATE 44100
#define BUFFER_SIZE 1024
#define WAV_BUFFER_COUNT (BUFFER_SIZE / 4 / sizeof(int16_t))
#define RECORDING_SAMPLES 65536 * 4

#define TASK_STACK 4096

#define MOUNT_POINT "/sdcard"

QueueHandle_t recPlayQueue;
TaskHandle_t recPlayManagerTaskHandle;

SemaphoreHandle_t recPlayMgrReady;
SemaphoreHandle_t recSem;
SemaphoreHandle_t playSem;

bool recPlayMgrError = false;

typedef enum { RECORD, PLAY, REC_STOP, PLAY_STOP, END } CmdType;
typedef struct {
    CmdType type;
    char filename[16];
} RecPlayCommand;

char recFileName[32];
char playFileName[32];
volatile bool recContinue;
volatile bool playContinue;

char buffer[BUFFER_SIZE];
int16_t wavBuffer[WAV_BUFFER_COUNT];
int recIndex = 0;

// .wav_size and .data_bytes still needed
wav_header WAVHeader = {
    .riff_header = { 'R', 'I', 'F', 'F' },
    .wave_header = { 'W', 'A', 'V', 'E' },
    .wav_size = 0, // to be rewritten
    // Format Header
    .fmt_header = { 'f', 'm', 't', ' ' },
    .fmt_chunk_size = 16,
    .audio_format = 1,
    .num_channels = 1,
    .sample_rate = SAMPLING_RATE,
    .byte_rate = SAMPLING_RATE * 2,
    .sample_alignment = 2,
    .bit_depth = 16,
    // Data
    .data_header = { 'd', 'a', 't', 'a' },
    .data_bytes = 0 // to be rewritten
};

i2s_chan_handle_t getMic() {
    i2s_chan_handle_t rx_handle;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &rx_handle);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLING_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DOUT,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    i2s_channel_init_std_mode(rx_handle, &std_cfg);
    return rx_handle;
}

i2s_chan_handle_t getAmp() {
    i2s_chan_handle_t tx_handle;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLING_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AMP_BCLK,
            .ws = AMP_WS,
            .dout = AMP_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    i2s_channel_init_std_mode(tx_handle, &std_cfg);
    return tx_handle;
}


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
        exit(1);
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
        exit(1);
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



void recorderTask(void *pvParameters) {
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    
    i2s_chan_handle_t micHandle = getMic();
    size_t bytesRead = 0;
    
    // Read and discard, so we can get stable value
    printf("Starting mic\n");
    i2s_channel_enable(micHandle);

    while (true) {
        // wait until called for
        xSemaphoreTake(recSem, portMAX_DELAY);
        
        ESP_LOGI("recorder", "Opening file %s", recFileName);
        FILE *f = fopen(recFileName, "w");
        if (f == NULL) {
            ESP_LOGE("recorder", "Failed to open file for writing");
            return;
        }
        // Leave some place for header
        fseek(f, sizeof(wav_header), SEEK_SET);
        
        // time to get average value, which can be subtracted later
        int64_t bias = 0;
        const int time = 100;
        for (int i=0; i < (SAMPLING_RATE * 8 * time / BUFFER_SIZE / 1000); i++) {
            i2s_channel_read(micHandle, buffer, BUFFER_SIZE, &bytesRead, 1000);
            
            int bufferIndex = 0;
            while (bufferIndex < bytesRead) {
                int32_t *ptr = (int32_t *)(buffer + bufferIndex + 4);
                bias += *ptr;
                bufferIndex += 8;
            }
        }
        bias /= SAMPLING_RATE * time / 1000;
        printf("Bias: %lld\n", bias);
        
        printf("Starting recording...\n");
        gpio_set_level(LED_PIN, 1);
        
        
        while (recContinue) {
            i2s_channel_read(micHandle, buffer, BUFFER_SIZE, &bytesRead, 1000);
            
            int bufferIndex = 0;
            int wavBufferIndex = 0;
            while (bufferIndex < bytesRead) {
                // get pointer to the left channel 32-bit sample
                int32_t *ptr = (int32_t *)(buffer + bufferIndex + 4);
                // do the magic
                int16_t sample = (*ptr - bias) >> 14;
                wavBuffer[wavBufferIndex] = sample;
                // skip 4 bytes for each channel
                bufferIndex += 8;
                wavBufferIndex++;
                recIndex++;
            }
            fwrite(wavBuffer, sizeof(wavBuffer), 1, f);
        }
        printf("Ending recording...\n");
        gpio_set_level(LED_PIN, 0);
        
        rewind(f);
        
        WAVHeader.data_bytes = 2 * recIndex;
        WAVHeader.wav_size = WAVHeader.data_bytes + sizeof(WAVHeader) - 8; 
        fwrite(&WAVHeader, sizeof(WAVHeader), 1, f);
        fclose(f);
    }
    // this will never happen but whatever
    i2s_channel_disable(micHandle);
    i2s_del_channel(micHandle);
}

void printBuffer(void *pvParameters) {
    char *currentBuffer = buffer;
    for (int i=0; i<BUFFER_SIZE; i++) {
        if (i % 8 == 0) {
            printf("\n");
        }
        
        if (i % 4 == 0) {
            int16_t *ptr = (int16_t *)(currentBuffer + i + 2);
            printf(" (%x) ", *ptr);
        }
        
        printf(" %x ", currentBuffer[i]);
    }
}


void playerTask() {
    i2s_chan_handle_t ampHandle = getAmp();
    
    while (1) {
        xSemaphoreTake(playSem, portMAX_DELAY);
        
        ESP_LOGI("sdcard", "Opening file %s", playFileName);
        FILE *f = fopen(playFileName, "r");
        if (f == NULL) {
            ESP_LOGE("sdcard", "Failed to open file for reading");
            return;
        }
        wav_header fileHeader;
        fread(&fileHeader, sizeof(wav_header), 1, f);
        // check header compatible

        memset(buffer, 0, BUFFER_SIZE);
        size_t bytesWritten = 0;
        int samplesRead = 0;
        
        printf("Starting playback...\n");
        i2s_channel_enable(ampHandle);
        while (playContinue) {
            samplesRead = fread(wavBuffer, sizeof(int16_t), WAV_BUFFER_COUNT, f);
            if (samplesRead == 0) {
                break;
            }
            int bufferIndex = 0;
            int wavBufferIndex = 0;
            while (wavBufferIndex < samplesRead) {
                int16_t *ptr = (int16_t *)(buffer + bufferIndex + 6);
                *ptr = wavBuffer[wavBufferIndex];
                bufferIndex += 8;
                wavBufferIndex++;
            }
            i2s_channel_write(ampHandle, buffer, BUFFER_SIZE, &bytesWritten, 1000);
        }
        
        i2s_channel_disable(ampHandle);
        fclose(f);
    }
    
    i2s_del_channel(ampHandle);
}

void nameToPath(char *path, char *name) {
    strcpy(path, MOUNT_POINT);
    strcat(path, "/");
    strcat(path, name);
    strcat(path, ".wav");
}


void recPlayManagerTask(void *pvParameters) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdmmc_card_t *card = mountSD(&host);
    
    recSem = xSemaphoreCreateBinary();
    playSem = xSemaphoreCreateBinary();
    
    TaskHandle_t recorderTaskHandle, playerTaskHandle;
    xTaskCreate(recorderTask, "RECORDER", TASK_STACK, NULL, 1, &recorderTaskHandle);
    xTaskCreate(playerTask, "PLAYER", TASK_STACK, NULL, 1, &playerTaskHandle);
    
    RecPlayCommand cmd;
    
    while (xQueueReceive(recPlayQueue, &cmd, portMAX_DELAY)) {
        switch (cmd.type) {
            case RECORD:
                ESP_LOGI("recplaymgr", "Starting recording '%s'\n", cmd.filename);
                recContinue = true;
                nameToPath(recFileName, cmd.filename);
                xSemaphoreGive(recSem);
                break;

            case PLAY:
                ESP_LOGI("recplaymgr", "Replaying '%s'\n", cmd.filename);
                playContinue = true;
                nameToPath(playFileName, cmd.filename);
                xSemaphoreGive(playSem);
                break;
            
            case REC_STOP:
                ESP_LOGI("recplaymgr", "Ending recording\n");
                recContinue = false;
                break;
                
            case PLAY_STOP:
                ESP_LOGI("recplaymgr", "Ending playback\n");
                playContinue = false;
                break;
                
            case END:
                ESP_LOGI("recplaymgr", "Ending activity, unmounting SD card\n");
                recContinue = false;
                playContinue = false;
                vTaskDelay(10 / portTICK_PERIOD_MS);
                unmountSD(card, &host);
                vTaskDelay(portMAX_DELAY);
                break;
        }
    }
}

void app_main() {
    recPlayQueue = xQueueCreate(4, sizeof(RecPlayCommand));
    
    xTaskCreate(recPlayManagerTask, "REC_PLAY_MGR", TASK_STACK, NULL, 1, &recPlayManagerTaskHandle);

    
    
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    
    RecPlayCommand cmd = {RECORD, "1"};
    xQueueSend(recPlayQueue, &cmd, 0);
    vTaskDelay(4000 / portTICK_PERIOD_MS);
    
    cmd.type = REC_STOP;
    xQueueSend(recPlayQueue, &cmd, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    cmd.type = PLAY;
    xQueueSend(recPlayQueue, &cmd, 0);
    vTaskDelay(2500 / portTICK_PERIOD_MS);
    
    cmd.type = END;
    xQueueSend(recPlayQueue, &cmd, 0);
    
}
