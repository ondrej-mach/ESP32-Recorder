
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

#define SD_MISO GPIO_NUM_19
#define SD_MOSI GPIO_NUM_23
#define SD_CLK GPIO_NUM_18
#define SD_CS GPIO_NUM_5

#define LED_PIN GPIO_NUM_14

#define SAMPLING_RATE 44100
#define BUFFER_SIZE 1024
#define WAV_BUFFER_COUNT (BUFFER_SIZE / 4 / sizeof(int16_t))
#define RECORDING_SAMPLES 65536 * 4

#define MOUNT_POINT "/sdcard"

char buffer[BUFFER_SIZE];
//int16_t recording[RECORDING_SAMPLES];
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



void record(FILE *f) {
    i2s_chan_handle_t micHandle = getMic();

    char *currentBuffer = buffer;
    size_t bytesRead = 0;
    
    // Leave some place for header
    fseek(f, sizeof(wav_header), SEEK_SET);
    
    // Read and discard for a while
    printf("Starting mic\n");
    i2s_channel_enable(micHandle);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    // time to get average value, which can be subtracted later
    int64_t bias = 0;
    const int time = 100;
    for (int i=0; i < (SAMPLING_RATE * 8 * time / BUFFER_SIZE / 1000); i++) {
        i2s_channel_read(micHandle, currentBuffer, BUFFER_SIZE, &bytesRead, 1000);
        
        int bufferIndex = 0;
        while (bufferIndex < bytesRead) {
            int32_t *ptr = (int32_t *)(currentBuffer + bufferIndex + 4);
            bias += *ptr;
            bufferIndex += 8;
        }
    }
    bias /= SAMPLING_RATE * time / 1000;
    printf("Bias: %lld\n", bias);
    
    printf("Starting recording...\n");
    while (recIndex < RECORDING_SAMPLES) {
        i2s_channel_read(micHandle, currentBuffer, BUFFER_SIZE, &bytesRead, 1000);
        
        int bufferIndex = 0;
        int wavBufferIndex = 0;
        while ((recIndex < RECORDING_SAMPLES) && (bufferIndex < bytesRead)) {
            // get pointer to the left channel 32-bit sample
            int32_t *ptr = (int32_t *)(currentBuffer + bufferIndex + 4);
            // do the magic
            int16_t sample = (*ptr - bias) >> 14;
            // recording[recIndex] = sample;
            wavBuffer[wavBufferIndex] = sample;
            // skip 4 bytes for each channel
            bufferIndex += 8;
            wavBufferIndex++;
            recIndex++;
        }
        fwrite(wavBuffer, sizeof(wavBuffer), 1, f);
    }
    printf("Ending recording...\n");
    i2s_channel_disable(micHandle);
    i2s_del_channel(micHandle);
    
    rewind(f);
    
    WAVHeader.data_bytes = 2 * recIndex;
    WAVHeader.wav_size = WAVHeader.data_bytes + sizeof(WAVHeader) - 8; 
    fwrite(&WAVHeader, sizeof(WAVHeader), 1, f);
}

void printBuffer() {
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


void playback(FILE *f) {
    wav_header fileHeader;
    fread(&fileHeader, sizeof(wav_header), 1, f);
    // check header compatible
    
    char *currentBuffer = buffer;
    
    i2s_chan_handle_t ampHandle = getAmp();
    
    memset(currentBuffer, 0, BUFFER_SIZE);
    size_t bytesWritten = 0;
    
    i2s_channel_enable(ampHandle);
    
    printf("Starting playback...\n");
    
    int samplesRead = 0;
    while ((samplesRead = fread(wavBuffer, sizeof(int16_t), WAV_BUFFER_COUNT, f)) != 0) {
        
        int bufferIndex = 0;
        int wavBufferIndex = 0;
        while (wavBufferIndex < samplesRead) {
            int16_t *ptr = (int16_t *)(currentBuffer + bufferIndex + 6);
            *ptr = wavBuffer[wavBufferIndex];
            bufferIndex += 8;
            wavBufferIndex++;
        }
        i2s_channel_write(ampHandle, currentBuffer, BUFFER_SIZE, &bytesWritten, 1000);
    }
    
    i2s_channel_disable(ampHandle);
    i2s_del_channel(ampHandle);
}


void app_main() {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdmmc_card_t *card = mountSD(&host);
    
    const char *filename = MOUNT_POINT "/rec.wav";
    FILE *f;
    
    ESP_LOGI("sdcard", "Opening file %s", filename);
    f = fopen(filename, "w");
    if (f == NULL) {
        ESP_LOGE("sdcard", "Failed to open file for writing");
        return;
    }
    record(f);
    fclose(f);
    
    ESP_LOGI("sdcard", "Opening file %s", filename);
    f = fopen(filename, "r");
    if (f == NULL) {
        ESP_LOGE("sdcard", "Failed to open file for reading");
        return;
    }
    playback(f);
    fclose(f);

    unmountSD(card, &host);
    
    //printBuffer();

}
