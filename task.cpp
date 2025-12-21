#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "task.h"
#include <LittleFS.h>
#include "Arduino_GFX_Library.h"
#include <JPEGDEC.h>
#include "SensorQMI8658.hpp"

extern Arduino_RGB_Display *gfx;
extern const uint8_t *g_videoDataPtr; 
extern size_t g_videoSize;
extern uint8_t *frameBuf;


/*imu-sensor variables*/
extern SensorQMI8658 qmi;
IMUdata acc; // We need Accelerometer data now!
IMUdata gyr; 
// A "flat" device usually reads Z ~= 1.0g (give or take minimal noise)
// We define a "Stable Flat" range. Anything outside this means it's being held/tilted.
#define FLAT_Z_MIN -1.25f
#define FLAT_Z_MAX -0.8f

// To prevent flickering when passing through the threshold, we use a timer
#define STABLE_TIME_REQ  500   // Must be flat for 0.5s to turn BLUE
#define PICKUP_TIME_REQ  100   // Must be non-flat for 0.1s to turn RED (filters quick bumps)

volatile bool is_picked_up = false;      
uint32_t state_change_timer = 0; // Timer to track duration of current physical state

enum PlayerState { PLAY, PAUSE };

MjpegClass mjpegPlayer;



/* ----------------- 播放相关设置 ----------------- */
// const char *MJPEG_PATH = "/out.mjpeg";   // LittleFS 上的视频文件名（raw MJPEG stream）
const uint32_t TARGET_FPS = 10;           // 目标帧率，可按需调整（8~15 推荐）
const uint32_t FRAME_INTERVAL_MS = 1000UL / TARGET_FPS;

//JPEGDEC回调：GFX填像素
int jpegDrawCallback(JPEGDRAW *pDraw)
{
  // x,y,w,h, pPixels为RGB565 LE格式指针
  if (gfx)
  {
    int x_offset = 60;
    int y_offset = 60;
    //gfx->draw16bitBeRGBBitmap(pDraw->x+x_offset, pDraw->y+y_offset, (uint16_t *)pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, (uint16_t *)pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  }
  // yield();
  return 1; // 成功

}

// 将 8-bit RGB 转为 RGB565 (uint16_t)
static inline uint16_t rgbTo565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

// 生成灰度颜色（v 在 min..max）
static inline uint16_t grayTo565(uint8_t v) {
  return rgbTo565(v, v, v);
}

// void drawSnow(Arduino_RGB_Display *gfx, uint32_t width, uint32_t height) {
//   if (!gfx) return;

//   // 每次调用绘制的雪花数量（可调，值越小越慢）
//   const uint32_t PIXELS_TO_DRAW = (width * height) / 2000; // 480x480 -> ~115 -> 试试 /2000 或 /3000 更慢
//   const uint8_t FLAKE_SIZE = 10; // 雪花方块大小，原来为10

//   for (uint32_t i = 0; i < PIXELS_TO_DRAW; i++) {
//     uint16_t x = esp_random() % width;
//     uint16_t y = esp_random() % height;

//     // 选择颜色类别：白 / 灰 / 黑（按概率）
//     uint32_t r = esp_random() % 100; // 0..99
//     uint16_t color;
//     if (r < 30) {
//       // 白色系（亮灰，高亮度）
//       // 亮度范围 220..255
//       uint8_t v = 220 + (esp_random() % 36); // 220..255
//       color = grayTo565(v);
//     } else if (r < 90) {
//       // 灰色系（中间灰度）
//       // 亮度范围 40..200，可根据需要调整为更暗或更亮
//       uint8_t v = 40 + (esp_random() % 161); // 40..200
//       color = grayTo565(v);
//     } else {
//       // 黑色系（深灰/黑）
//       uint8_t v = esp_random() % 40; // 0..39
//       color = grayTo565(v);
//     }

//     gfx->fillRect(x, y, FLAKE_SIZE, FLAKE_SIZE, color);

//     // 每若干个雪花让出一次 CPU，避免阻塞或触发看门狗
//     if ((i & 0xF) == 0) { // 每 16 个短暂让出一次
//       vTaskDelay(pdMS_TO_TICKS(20)); // 可改为 1~20ms 调整速度与流畅度
//     }
//   }

//   // 如果需要可以在这里调用 gfx->flush();（视后端而定）
// }

uint16_t snowColors[3] = {
  0xFFFF,  // 白
  grayTo565(128),  // 灰（中间亮度）
  0x0000   // 黑
};
const uint8_t GRID_SIZE = 12;  // 12x12
const uint16_t BLOCK_SIZE = 40;  // 40x40


void drawSnow(Arduino_RGB_Display *gfx, uint32_t width, uint32_t height){
  // 每次更新块的数量（可调，值越小更新越慢）
  const uint8_t BLOCKS_TO_UPDATE = 16;  // 每次更新 20 块


  for(int i=0;i < BLOCKS_TO_UPDATE; i++){
  uint8_t row = esp_random() % GRID_SIZE;
  uint8_t col = esp_random() % GRID_SIZE;

  uint16_t x = col * BLOCK_SIZE;
  uint16_t y = row * BLOCK_SIZE;

  uint16_t color = snowColors[esp_random() % 3];

  gfx->fillRect(x, y, BLOCK_SIZE, BLOCK_SIZE, color);
    
  }
  vTaskDelay(pdMS_TO_TICKS(20));

}



void mjpegplayer(void *pvParameters){
  if (!g_videoDataPtr || g_videoSize == 0) {
    Serial.println("ERROR: Video data not loaded into PSRAM. Exiting task.");
    vTaskDelete(NULL); 
    return;
  }

  uint32_t framesPlayed = 0, droppedFrames = 0;
  unsigned long playbackStart = millis();
  uint32_t frameCount = 0;
  PlayerState state = PAUSE;
  const TickType_t xFrequency = pdMS_TO_TICKS(FRAME_INTERVAL_MS);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  while (true) {
    switch (state) {
      case PAUSE:
        if (is_picked_up) {
          vTaskDelay(pdMS_TO_TICKS(1000));
          if(is_picked_up){
            state = PLAY;
            playbackStart = millis();  
            frameCount = 0;  
            framesPlayed = 0;
            droppedFrames = 0;
            mjpegPlayer.resetIndex();  
            Serial.println("Video playback started");
            }  
        }
        else {         
          drawSnow(gfx, 480, 480);
          vTaskDelay(pdMS_TO_TICKS(300));
          xLastWakeTime = xTaskGetTickCount(); 
        }
        break;

      case PLAY:
        if (is_picked_up) {
          bool ok = mjpegPlayer.readMjpegBuf();
          if (!ok) {
            // 视频播放完毕，重置到开头（循环播放）
            mjpegPlayer.resetIndex();
            // 如果需要延迟1秒再循环，添加这里：
            vTaskDelay(pdMS_TO_TICKS(1000));
          }
          else {
            // 解码和显示
            unsigned long decodeStart = millis();
            mjpegPlayer.drawJpg();
            // gfx->flush();  // 如果需要，取消注释
            framesPlayed++;
            frameCount++;
            unsigned long decodeCost = millis() - decodeStart;
            if (decodeCost > FRAME_INTERVAL_MS) {
            }
            // 控制帧率
            vTaskDelayUntil(&xLastWakeTime, xFrequency);
          }
        } 
        else {
          // 切换到暂停：打印统计
          state = PAUSE;
          uint32_t playTime = millis() - playbackStart;
          Serial.printf("Video paused. framesPlayed=%u dropped=%u time=%ums\n", framesPlayed, droppedFrames, playTime);
        }
        break;
    }
  }



}


void imusensor(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  while(true){
    if (qmi.getDataReady()) {
    // Get Accelerometer data (x, y, z in g's)
    if (qmi.getAccelerometer(acc.x, acc.y, acc.z)) {
      bool currently_flat = (acc.z > FLAT_Z_MIN && acc.z < FLAT_Z_MAX) && (abs(acc.x) < 0.2 && abs(acc.y) < 0.2);

      // --- State Machine ---     
      if (currently_flat) {
        if (is_picked_up) {
            if (millis() - state_change_timer > STABLE_TIME_REQ) {
                is_picked_up = false;
                //gfx->fillScreen(BLUE);
                Serial.println("Action: PUT DOWN (Stable Flat)");
                state_change_timer = millis(); 
            }
        } else {
          
            state_change_timer = millis(); 
        }

      } else {

        if (!is_picked_up) {

            if (millis() - state_change_timer > PICKUP_TIME_REQ) {
                is_picked_up = true;
                //gfx->fillScreen(RED);
                Serial.println("Action: PICKED UP");
                state_change_timer = millis(); 
            }
        } else {
              state_change_timer = millis();
          }
        }
      }
    }
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
  }
}




