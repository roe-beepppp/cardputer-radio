#pragma once
#include <Arduino.h>
#include <vector>
#include "player.h"

namespace radio {

enum Status : uint8_t { OFF = 0, CONNECTING, CONNECTED, FAILED };

bool enter();
void leave();
void tick();
uint8_t status();
const char* statusText();
bool loadStations(std::vector<player::RadioStation>& out);
int httpsSkipped();

}
