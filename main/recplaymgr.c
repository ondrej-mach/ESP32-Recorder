// Some of the code in this file is taken from ESP-IDF I2S example

#include "recplaymgr.h"

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

#define MIC_DOUT GPIO_NUM_16
#define MIC_BCLK GPIO_NUM_17
#define MIC_WS GPIO_NUM_25

#define AMP_DIN GPIO_NUM_2
#define AMP_BCLK GPIO_NUM_4
#define AMP_WS GPIO_NUM_27

#define LED_PIN GPIO_NUM_26

#define SAMPLING_RATE 44100
#define BUFFER_SIZE 1024
#define WAV_BUFFER_COUNT (BUFFER_SIZE / 4 / sizeof(int16_t))
#define RECORDING_SAMPLES 65536 * 4

#define FILENAME_LEN 32

#define TASK_STACK 4096

typedef enum { RECORD, PLAY, REC_STOP, PLAY_STOP, END } CmdType;
typedef struct {
    CmdType type;
    char filename[FILENAME_LEN];
} RecPlayCommand;

QueueHandle_t recPlayQueue;
TaskHandle_t recPlayManagerTaskHandle;

SemaphoreHandle_t recSem;
SemaphoreHandle_t playSem;

volatile bool recPlayMgrError = false;

static char recFileName[FILENAME_LEN];
static char playFileName[FILENAME_LEN];
static volatile bool recContinue;
static volatile bool playContinue;

static char buffer[BUFFER_SIZE];
static int16_t wavBuffer[WAV_BUFFER_COUNT];

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
        recContinue = true;
        
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
        
        ESP_LOGI("recorder", "Starting recording");
        gpio_set_level(LED_PIN, 1);
        
        int wavTotalSamples = 0;
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
            }
            fwrite(wavBuffer, sizeof(wavBuffer), 1, f);
            wavTotalSamples += wavBufferIndex;
        }
        ESP_LOGI("recorder", "Ending recording");
        gpio_set_level(LED_PIN, 0);
        
        rewind(f);
        
        WAVHeader.data_bytes = 2 * wavTotalSamples;
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
        playContinue = true;
        
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
        
        ESP_LOGI("player", "Starting playback");
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
        ESP_LOGI("player", "Playback ended");
    }
    i2s_del_channel(ampHandle);
}


void recPlayMgrInit() {
    recSem = xSemaphoreCreateBinary();
    playSem = xSemaphoreCreateBinary();
    
    xTaskCreate(recorderTask, "RECORDER", TASK_STACK, NULL, 1, NULL);
    xTaskCreate(playerTask, "PLAYER", TASK_STACK, NULL, 1, NULL);
}

void startRec(char *filename) {
    ESP_LOGI("recplaymgr", "Starting recording '%s'", filename);
    playContinue = false;
    recContinue = false;
    strcpy(recFileName, filename);
    xSemaphoreGive(recSem);
}

void startPlay(char *filename) {
    ESP_LOGI("recplaymgr", "Replaying '%s'", filename);
    playContinue = false;
    recContinue = false;
    strcpy(playFileName, filename);
    xSemaphoreGive(playSem);
}

void stopRec() {
    ESP_LOGI("recplaymgr", "Ending recording");
    recContinue = false;
}
    
void stopPlay() {
    ESP_LOGI("recplaymgr", "Ending playback");
    playContinue = false;
}
