/*
 * MjpegClass - MJPEG Video Playback Class for ESP32
 *
 * This code is heavily modified and adapted from the original work:
 *
 * Original Source Repository：
 * [https://github.com/moononournation/RGB565_video]
 *
 * * Original Author:
 * [moononournation]
 *
 * Original License:
 * 
 * Modifications by:
 * [Kaswish]
 * Date:
 * [18.12.2025]
 * Description of Major Changes:
 * 1. 替换了基于 LittleFS/File Stream 的 I/O 逻辑。
 * 2. 实现了基于 PSRAM 内存指针 (_memData) 的视频数据读取。
 * 3. 优化了帧搜索和内存拷贝操作。
 */

#ifndef TASK_H
#define TASK_H
#include <Arduino.h>
#include <JPEGDEC.h>
// #include <Wire.h>
// #include "Arduino_GFX_Library.h"

#define READ_BUFFER_SIZE (64*1024)

extern int jpegDrawCallback(JPEGDRAW *pDraw);
class MjpegClass
{
public:
  bool setup(
     const uint8_t *data_ptr, size_t data_len, // <-- 新增/修改：内存播放的接口
        uint8_t *mjpeg_buf, JPEG_DRAW_CALLBACK *pfnDraw, bool useBigEndian,
        int x, int y, int widthLimit, int heightLimit)
    {
        // _input = input; // <-- 删除
        _memData = data_ptr;    // <-- 新增：记录内存指针
        _memDataLen = data_len; // <-- 新增：记录总长度

        _mjpeg_buf = mjpeg_buf;
        _pfnDraw = pfnDraw;
        _useBigEndian = useBigEndian;
        _x = x;
        _y = y;
        _widthLimit = widthLimit;
        _heightLimit = heightLimit;
        _inputindex = 0; // 从内存数据起始处开始播放

        // 仍然需要 _read_buf 进行数据块缓冲和搜索
        _read_buf = (uint8_t *)ps_malloc(READ_BUFFER_SIZE);
        if (!_read_buf) return false; // 检查分配是否成功

      

        return true;
  }

  bool readMjpegBuf()
  {
    // --- 辅助函数：内存读取 (替代 _input->readBytes) ---
    // 从内存中读取数据到 _read_buf 的指定位置
    auto read_from_memory = [&](uint8_t* target_buf, size_t target_size) -> size_t {
        // 1. 计算剩余可读字节数
        size_t available = _memDataLen - _inputindex;
        size_t to_read = (available < target_size) ? available : target_size;
        
        // 2. 复制数据
        if (to_read > 0) {
            memcpy(target_buf, _memData + _inputindex, to_read);
            _inputindex += to_read;
        }
        return to_read;
    };
    // ----------------------------------------------------


    if (_inputindex == 0)
    {
        // 第一次读取，从内存起始处填充 _read_buf
        _buf_read = read_from_memory(_read_buf, READ_BUFFER_SIZE);
         yield();
    }
    _mjpeg_buf_offset = 0;
    int i = 0;
    bool found_FFD8 = false;
    
    // --- 寻找 FFD8 逻辑 (保持不变，但 i/o 替换) ---
    while ((_buf_read > 0) && (!found_FFD8))
    {
        i = 0;
        while ((i < _buf_read) && (!found_FFD8))
        {
            if ((_read_buf[i] == 0xFF) && (_read_buf[i + 1] == 0xD8))
            {
                found_FFD8 = true;
            }
            ++i;
        }
        if (found_FFD8)
        {
            --i;
        }
        else
        {
            // 替换 I/O：如果当前块找不到 FFD8，则丢弃当前块，读取下一个块
            _buf_read = read_from_memory(_read_buf, READ_BUFFER_SIZE); // <-- 替换 I/O
            yield();
            // 如果读取失败 (_buf_read == 0)，则退出循环，返回 false
        }
    }    
   
    uint8_t *_p = _read_buf + i;
    _buf_read -= i;
    bool found_FFD9 = false;
    
    // --- 寻找 FFD9 逻辑 (保持不变，但 i/o 替换) ---
    if (_buf_read > 0)
    {
        i = 3; // i 重新用于内部搜索指针
        while ((_buf_read > 0) && (!found_FFD9))
        {
            // ... (您的 FFD9 搜索逻辑保持不变) ...
          if ((_mjpeg_buf_offset > 0) && (_mjpeg_buf[_mjpeg_buf_offset - 1] == 0xFF) && (_p[0] == 0xD9))
          {
            found_FFD9 = true;
          }
          else
          {
            while ((i < _buf_read) && (!found_FFD9))
            {
              if ((_p[i] == 0xFF) && (_p[i + 1] == 0xD9))
              {
                found_FFD9 = true;
                ++i;
              }
              ++i;
            }
          }
            // 核心 I/O 替换点 1 (在 else 块中)
            /* 原始代码: 
                else {
                  while ((i < _buf_read) && (!found_FFD9)) { ... }
                }
            */

            // 拷贝数据到 _mjpeg_buf (保持不变)
            memcpy(_mjpeg_buf + _mjpeg_buf_offset, _p, i);
            _mjpeg_buf_offset += i;
            
            // 核心 I/O 替换点 2
            size_t o = _buf_read - i;
            if (o > 0)
            {
                // 移位：将下一帧的起始数据移到 _read_buf 头部
                memcpy(_read_buf, _p + i, o);
                
                // 替换 I/O：从内存中读取数据，填充剩余空间
                _buf_read = read_from_memory(_read_buf + o, READ_BUFFER_SIZE - o); // <-- 替换 I/O
                yield();
                _p = _read_buf;
                _buf_read += o; // 加上移位数据的长度
            }
            else // o == 0
            {
                // 替换 I/O：完整读取下一个数据块
                _buf_read = read_from_memory(_read_buf, READ_BUFFER_SIZE); // <-- 替换 I/O
                yield();
                _p = _read_buf;
            }
            i = 0; // 重置内部搜索指针 i
        }
        if (found_FFD9)
        {
            return true;
        }
    }
    return false; // 到达内存末尾，没有找到完整的 FFD9
}

  bool drawJpg()
  {
    _remain = _mjpeg_buf_offset;
    _jpeg.openRAM(_mjpeg_buf, _remain, _pfnDraw);
    if (_scale == -1)
    {
      // scale to fit height
      int iMaxMCUs;
      int w = _jpeg.getWidth();
      int h = _jpeg.getHeight();
      float ratio = (float)h / _heightLimit;
      if (ratio <= 1)
      {
        _scale = 0;
        iMaxMCUs = _widthLimit / 16;
      }
      else if (ratio <= 2)
      {
        _scale = JPEG_SCALE_HALF;
        iMaxMCUs = _widthLimit / 8;
        w /= 2;
        h /= 2;
      }
      else if (ratio <= 4)
      {
        _scale = JPEG_SCALE_QUARTER;
        iMaxMCUs = _widthLimit / 4;
        w /= 4;
        h /= 4;
      }
      else
      {
        _scale = JPEG_SCALE_EIGHTH;
        iMaxMCUs = _widthLimit / 2;
        w /= 8;
        h /= 8;
      }
      _jpeg.setMaxOutputSize(iMaxMCUs);
      _x = (w > _widthLimit) ? 0 : ((_widthLimit - w) / 2);
      _y = (_heightLimit - h) / 2;
    }
    if (_useBigEndian)
    {
      _jpeg.setPixelType(RGB565_BIG_ENDIAN);
    }
    _jpeg.decode(_x, _y, _scale);
    _jpeg.close();

    return true;
  }
 void resetIndex() {
        _inputindex = 0;
    }
private:
  const uint8_t *_memData;      // <-- 新增：指向预加载视频数据起始的指针
  size_t _memDataLen;         // <-- 新增：总视频数据长度
 
  
  uint8_t *_mjpeg_buf;
  JPEG_DRAW_CALLBACK *_pfnDraw;
  bool _useBigEndian;
  int _x;
  int _y;
  int _widthLimit;
  int _heightLimit;

  uint8_t *_read_buf;
  int32_t _mjpeg_buf_offset = 0;

  JPEGDEC _jpeg;
  int _scale = -1;

  int32_t _inputindex = 0; // 现在是内存数据的偏移量
  int32_t _buf_read;       // 仍然用于记录 _read_buf 中的有效字节数
  int32_t _remain = 0;
};



void imusensor(void *pvParameters);
void mjpegplayer(void *pvParameters);

extern MjpegClass mjpegPlayer;
extern volatile bool is_picked_up;    

 #endif