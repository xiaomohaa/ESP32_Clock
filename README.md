# ESP32-C6 AMOLED Clock

基于 Waveshare ESP32-C6-Touch-AMOLED-2.16 开发板的时钟项目，支持自动四方向旋转。

## 硬件

- **MCU**: ESP32-C6 (512KB HP SRAM, RISC-V)
- **屏幕**: 2.16" AMOLED 480x480, SH8601 驱动, QSPI 接口
- **触摸**: CST9217 电容触摸, I2C 接口
- **传感器**: QMI8658 6轴 IMU (加速度计 + 陀螺仪), I2C 接口
- **电源**: AXP2101 PMIC

## 软件框架

- **ESP-IDF**: v6.0.1
- **LVGL**: v8.4
- **显示驱动**: `espressif/esp_lcd_sh8601`
- **触摸驱动**: `waveshare/esp_lcd_touch_cst9217`
- **IMU驱动**: `waveshare/qmi8658`

## 功能

- 彩虹色数字时钟 (时:分 + 秒)
- 日期 + 星期显示
- WiFi 自动连接 (支持多网络切换)
- NTP 时间同步 (ntp.aliyun.com)
- 电池电量显示 (带充电动画)
- WiFi 状态图标 (连接后常亮，未连接闪烁)
- **QMI8658 加速度计自动四方向旋转**

## 关键设置

### 屏幕旋转方向 (MADCTL)

文件: `components/port_bsp/display_bsp.cpp` → `Set_Rotate()` 函数

```cpp
static const uint8_t madctl[] = { 0x30, 0x00, 0x60, 0xC0 };
//  索引:                      rot=0  rot=1 rot=2 rot=3
//                            UP    RIGHT DOWN  LEFT
```

SH8601 显示控制器的 MADCTL 寄存器 (0x36) 值与标准 ILI9341 不同，以下为实测值：

| rot值 | MADCTL | 方向 | 说明 |
|-------|--------|------|------|
| 0     | 0x30   | UP   | 正常方向 |
| 1     | 0x00   | RIGHT | 向右旋转90° |
| 2     | 0x60   | DOWN | 旋转180° |
| 3     | 0xC0   | LEFT | 向左旋转90° |

其他可选 MADCTL 值参考 (通过 `MADCTL_TEST_MODE` 测试):
`0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0xF0, 0x30`

### MADCTL 测试模式

文件: `main/main.cpp` 第21行

```cpp
#define MADCTL_TEST_MODE 0  // 设为 1 启用测试模式
```

启用后，屏幕会每3秒自动切换一个 MADCTL 值，串口打印当前值。用于调试屏幕旋转方向：
1. 将设备转到目标方向
2. 观察屏幕，等待显示正确的那一刻
3. 查看串口输出的 `MADCTL TEST: 0xXX` 值
4. 将该值填入 `madctl[]` 数组对应位置

### 旋转检测参数

文件: `main/main.cpp` → `rotation_task()` 函数

- **采样间隔**: 100ms
- **低通滤波**: α=0.3 (`sx = sx * 0.7f + raw * 0.3f`)
- **触发阈值**: 6.87 m/s² (~0.7g)
- **连续确认次数**: 3次相同方向才触发旋转
- **I2C 超时**: 100ms (使用原始 `i2c_master_transmit_receive` 避免阻塞)

### 传感器方向映射

```cpp
if (ax > 6.87f && ax > ay) {
    new_rot = (sx > 0) ? 3 : 1;  // X正方向=LEFT(3), X负方向=RIGHT(1)
} else if (ay > 6.87f && ay > ax) {
    new_rot = (sy > 0) ? 0 : 2;  // Y正方向=UP(0), Y负方向=DOWN(2)
}
```

### 旋转时的 SPI/LVGL 同步

`Set_Rotate()` 必须在 `Lvgl_lock()/Lvgl_unlock()` 保护下调用，否则会与 LVGL 渲染产生 SPI 总线死锁：

```cpp
if (Lvgl_lock(1000) == ESP_OK) {
    user_display->Set_Rotate(cur_rot);
    Lvgl_unlock();
}
```

## 构建与烧录

```bash
# 激活 ESP-IDF 环境
source ~/.espressif/v6.0.1/esp-idf/export.sh

# 构建
idf.py build

# 烧录 (根据实际串口号修改)
idf.py -p /dev/cu.usbmodem1101 flash

# 串口监视器
idf.py -p /dev/cu.usbmodem1101 monitor
```

## 项目结构

```
├── main/
│   ├── main.cpp              # 主程序 (时钟UI, WiFi, NTP, 旋转任务)
│   └── idf_component.yml     # 组件依赖声明
├── components/
│   └── port_bsp/
│       ├── display_bsp.cpp   # 显示驱动 (SPI, MADCTL旋转, 背光)
│       ├── display_bsp.h
│       ├── i2c_bsp.cpp       # I2C 总线初始化
│       └── CMakeLists.txt
├── managed_components/       # 自动下载的组件 (不提交到git)
└── build/                    # 编译输出 (不提交到git)
```

## 注意事项

- `qmi8658.h` 会重定义 `M_PI`，需在 `#include "qmi8658.h"` 之后再 `#include <math.h>`
- ESP-IDF v6.0.1 的 I2C 驱动拆分到了 `esp_driver_i2c`，waveshare 组件需手动修改 CMakeLists.txt 添加依赖
- `managed_components/` 目录由 idf_component_manager 自动管理，无需提交到 git
