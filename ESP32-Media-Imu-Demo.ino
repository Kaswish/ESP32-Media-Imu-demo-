#include "freertos/FreeRTOS.h"
#include "task.h"
#include "taskaudio.h"
#include "pin_config.h"
#include <Wire.h>
#include <LittleFS.h>
#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "SensorQMI8658.hpp"


/*MJpeg player related variables*/
#define MJPEG_MAX_FRAME (256*1024)
uint8_t *frameBuf = nullptr;  // 内存缓冲区
const char *MJPEG_PATH = "/out.mjpeg";
uint8_t *g_videoDataPtr = nullptr;
size_t g_videoSize = 0;


/*IMU-Sensor related variables*/
SensorQMI8658 qmi;


/*ESP32-smart box pin configurations*/
/*based of waveshare demo*/
Arduino_XCA9554SWSPI *expander = new Arduino_XCA9554SWSPI(
  7,
  0,
  2,
  1,
  &Wire,
  0x20);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  17 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 9 /* PCLK */,
  10 /* B0 */, 11 /* B1 */, 12 /* B2 */, 13 /* B3 */, 14 /* B4 */,
  21 /* G0 */, 8 /* G1 */, 18 /* G2 */, 45 /* G3 */, 38 /* G4 */, 39 /* G5 */,
  40 /* R0 */, 41 /* R1 */, 42 /* R2 */, 2 /* R3 */, 1 /* R4 */,
  1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
  1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  480 /* width */, 480 /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */,
  expander, GFX_NOT_DEFINED /* RST */, st7701_type1_init_operations, sizeof(st7701_type1_init_operations));




void setup()
{
  Serial.begin(115200);
  delay(100);

  Wire.begin(47, 48);

  // LittleFS Init
  if (!LittleFS.begin())
  {
    Serial.println("LittleFS.begin() failed!");
    while (1) delay(1000);
  }
  Serial.println("LittleFS initialized successfully.");

  /*mjpeg播放前的准备*/
  File mjpegFile = LittleFS.open(MJPEG_PATH, "r");
  if (!mjpegFile)
  {
    Serial.printf("ERROR: Failed to open video file: %s\n", MJPEG_PATH);
    while (1) delay(1000);
  }

  g_videoSize = mjpegFile.size();
  Serial.printf("Video file size: %u bytes\n", g_videoSize);

  g_videoDataPtr = (uint8_t *)ps_malloc(g_videoSize);
  if (!g_videoDataPtr) {
    Serial.println("ERROR: ps_malloc for video data failed! Out of PSRAM?");
    mjpegFile.close();
    while (1) delay(1000);
  }
  Serial.println("PSRAM buffer allocated successfully.");

  size_t bytesRead = mjpegFile.readBytes((char*)g_videoDataPtr, g_videoSize);
  mjpegFile.close();

  if (bytesRead != g_videoSize) {
    Serial.printf("ERROR: Only read %u out of %u bytes! Data incomplete.\n", bytesRead, g_videoSize);
    free(g_videoDataPtr); // 释放内存
    g_videoDataPtr = nullptr;
    while (1) delay(1000);
  }

  Serial.println("Video preloaded successfully into PSRAM.");
  Serial.printf("PSRAM Address: 0x%08X\n", (uint32_t)g_videoDataPtr);

  frameBuf = (uint8_t *)ps_malloc(MJPEG_MAX_FRAME);
    if (!frameBuf) {
        Serial.println("ERROR: Failed to allocate frameBuf in PSRAM.");
        while(1) delay(1000); 
    }
  
  //mjpegPlayer.setup(g_videoDataPtr, g_videoSize, frameBuf, jpegDrawCallback, true, 0, 0, 360, 360);
  mjpegPlayer.setup(g_videoDataPtr, g_videoSize, frameBuf, jpegDrawCallback, true, 0, 0, 480, 480);
  
  /*gfx Init*/
  if (gfx) {
    gfx->begin();
    gfx->setRotation(0);
    gfx->fillScreen(0);
  }

  /*IMU-Sensor Init*/
  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, 47, 48)) {
    Serial.println("Failed to find QMI8658!");
    while (1) delay(1000);
  }

  // Enable Accelerometer (Crucial for gravity detection)
  qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz, SensorQMI8658::LPF_MODE_0);
  qmi.enableAccelerometer();

  Serial.println("Setup done. Place device flat on table.");


  /*Audio Init*/
  expander->pinMode(3, OUTPUT);
  expander->digitalWrite(3, HIGH);
  delay(200);
  audioinit();


  Serial.println("Setup done.");

  // xTaskCreate(imusensor, "imu-sensor", 2048, NULL, 1, NULL);
  // xTaskCreate(mjpegplayer, "play-mjpeg-video", 12288, NULL, 2, NULL);
  // xTaskCreate(audioplayer, "play-audio", 8192, NULL, 5, NULL);
  xTaskCreatePinnedToCore(mjpegplayer, "play-mjpeg-video", 12288, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(imusensor, "imu-sensor", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(audioplayer, "playaudio", 8192, NULL, 5, NULL, 0);

}

void loop() {
  
}