#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "taskaudio.h"
#include <LittleFS.h>
#include <Wire.h>
#include "pin_config.h"
#include "es8311.h"
#include "ESP_I2S.h"
#include "esp_check.h"
#include "task.h"
#include "freertos/event_groups.h"

// I2C pins (demo used 47,48)
#define I2C_PORT              0
#define ES8311_I2C_ADDR       ES8311_ADDRESS_0 // demo uses ES8311_ADDRRES_0
//#define EXAMPLE_SAMPLE_RATE   16000
#define WAV_PATH               "/track.wav"
#define I2S_NUM_CH            I2S_NUM_0
#define READ_BUF_SZ           1048
#define EXAMPLE_VOICE_VOLUME  80                   // [0-100]

es8311_handle_t es_handle = NULL;

enum PlayerState { PLAY, PAUSE };

typedef struct {
    int sampleRate;
    uint16_t bitsPerSample;
    uint16_t numChannels;
    uint32_t dataOffset;
    uint32_t dataSize;
} WavHeaderInfo;

WavHeaderInfo wavInfo;

I2SClass i2s;


static uint32_t readLE32(File &f) {
  uint8_t b[4];
  if (f.read(b,4) != 4) return 0;
  return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static uint16_t readLE16(File &f) {
  uint8_t b[2];
  if (f.read(b,2) != 2) return 0;
  return (uint16_t)b[0] | ((uint16_t)b[1]<<8);
}

// Minimal WAV header parser for PCM little-endian
bool parseWavHeader(File &f, WavHeaderInfo &info) {
  f.seek(0);
  char riff[4];
  if (f.readBytes(riff,4) != 4) return false;
  if (strncmp(riff, "RIFF", 4) != 0) return false;
  readLE32(f); // file size
  char wave[4];
  if (f.readBytes(wave,4) != 4) return false;
  if (strncmp(wave, "WAVE", 4) != 0) return false;

  while (f.available()) {
    char chunkId[5] = {0};
    if (f.readBytes(chunkId,4) != 4) break;
    uint32_t chunkSize = readLE32(f);
    uint32_t chunkDataPos = f.position();

    if (strncmp(chunkId, "fmt ", 4) == 0) {
      uint16_t audioFormat = readLE16(f);
      info.numChannels = readLE16(f);
      info.sampleRate = readLE32(f);
      readLE32(f); // byte rate
      readLE16(f); // block align
      info.bitsPerSample = readLE16(f);
      uint32_t readSoFar = f.position() - chunkDataPos;
      if (readSoFar < chunkSize) f.seek(chunkDataPos + chunkSize);
    } else if (strncmp(chunkId, "data", 4) == 0) {
      info.dataOffset = f.position();
      info.dataSize = chunkSize;
      return true;
    } else {
      f.seek(chunkDataPos + chunkSize);
    }
  }
  return false;
}

esp_err_t es8311_codec_init(void) {
  es_handle = es8311_create(0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, "ES8311", "create failed");

  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = wavInfo.sampleRate * 256,
    .sample_frequency = wavInfo.sampleRate
  };

  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_ERROR_CHECK(es8311_sample_frequency_config(es_handle, es_clk.mclk_frequency, es_clk.sample_frequency));
  //ESP_ERROR_CHECK(es8311_microphone_config(es_handle, false));
  ESP_ERROR_CHECK(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL));
  //ESP_ERROR_CHECK(es8311_microphone_gain_set(es_handle, EXAMPLE_MIC_GAIN));

  return ESP_OK;
}

void audioinit(){

  File f = LittleFS.open(WAV_PATH, "r"); 
  if (!f) {
    Serial.printf("Failed to open %s\n", WAV_PATH);
    while (1) delay(100);
  }

  if (!parseWavHeader(f, wavInfo)) {
    Serial.println("Failed to parse WAV header or 'data' chunk not found.");
    f.close();
    while (1) delay(100);
  }

  if (wavInfo.bitsPerSample != 16) {
    Serial.println("Only 16-bit PCM WAV supported in this example.");
    f.close();
    while (1) delay(100);
  }
   f.close();


  i2s.setPins(PIN_ES7210_BCLK, PIN_ES7210_LRCK, PIN_ES8311_DOUT, PIN_ES7210_DIN, PIN_ES7210_MCLK);
  if (!i2s.begin(I2S_MODE_STD, wavInfo.sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S init failed!");

  }
  if (es8311_codec_init() != ESP_OK) {
    Serial.println("ES8311 init failed!");
   
  }
}


void audioplayer(void *pvParameters){
  EventBits_t uxReturn;
  /*use state maschine*/
  File audio = LittleFS.open(WAV_PATH, "r");
  if (!audio) {
    vTaskDelete(NULL); 
    return;
  }
  
  uint8_t readBuf[READ_BUF_SZ];
  uint32_t bytesRemaining = wavInfo.dataSize;

  PlayerState state = PAUSE;

  while(true){
    switch(state){
      case PAUSE:
        es8311_voice_mute(es_handle, true);
        vTaskDelay(pdMS_TO_TICKS(300));
        if(is_picked_up){
          vTaskDelay(pdMS_TO_TICKS(1000));  
          if(is_picked_up){
            audio.seek(wavInfo.dataOffset);
            bytesRemaining = wavInfo.dataSize;
            state = PLAY;
          }
        }
        break;
      
      case PLAY:
        es8311_voice_mute(es_handle, false);
        if(is_picked_up){        
          if(bytesRemaining > 0){             
            size_t toRead = min((uint32_t)READ_BUF_SZ, bytesRemaining);
            int readLen = audio.read(readBuf, toRead);
            if (readLen <= 0){
              state = PAUSE;
              break;
            } 
            if (wavInfo.numChannels == 1) {     
              i2s.write(readBuf, readLen);        
            } else {
              Serial.println("audio是双通道音频");
            }
            bytesRemaining -= readLen;
          } 
          else{
              vTaskDelay(pdMS_TO_TICKS(2000));
              //vTaskDelay(pdMS_TO_TICKS(10));  // delay              
              /*use eventgropsync*/
               uxReturn = xEventGroupSync( xHandle,
                                    AudioTaskBit,
                                    ALL_SYNC_BITS,
                                    portMAX_DELAY );

              if((uxReturn & ALL_SYNC_BITS) == ALL_SYNC_BITS){              
                audio.seek(wavInfo.dataOffset);
                bytesRemaining = wavInfo.dataSize;
                 vTaskDelay(pdMS_TO_TICKS(100));
              }                      

              
          }                   
        }
        else{
          state = PAUSE;
        }
        break;      
    }
  }
  audio.close();
  vTaskDelete(NULL);
}
