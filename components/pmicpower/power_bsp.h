#pragma once

#include "i2c_bsp.h"

void Custom_PmicPortInit(I2cMasterBus *i2cbus,uint8_t dev_addr);
void Custom_PmicRegisterInit(void);
void Axp2101_isChargingTask(void *arg);

void Axp2101_SetAldo2(uint8_t vol);
void Axp2101_SetAldo3(uint8_t vol);

int  Axp2101_GetBatteryPercent(void);  // 0-100, or -1 if no battery
bool Axp2101_IsCharging(void);
void Axp2101_PowerOff(void);


