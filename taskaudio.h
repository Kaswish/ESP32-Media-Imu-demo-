#ifndef TASKAUDIO_H
#define TASKAUDIO_H

void audioinit();
void audioplayer(void *pvParameters);

// Define flag bit
#define MjpegTaskBit    (1<<0)
#define AudioTaskBit    (1<<1)
#define ALL_SYNC_BITS ( MjpegTaskBit | AudioTaskBit)

extern EventGroupHandle_t xHandle;

#endif