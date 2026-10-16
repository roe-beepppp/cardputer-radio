#pragma once
#include <stdint.h>

namespace ui {

void init();
void tick();
void invalidate();
void toastVolume(uint8_t vol);
void toastMsg(const char* msg);
void setTheme(uint8_t idx);
uint8_t theme();
const char* themeName(uint8_t idx);
void showError(const char* msg);
void setLowMem(bool low);

void staOpen(bool open);
bool staIsOpen();
void staMove(int d);
void staPage(int d);
int staSel();

}
