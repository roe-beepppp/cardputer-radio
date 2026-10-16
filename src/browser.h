#pragma once
#include <Arduino.h>
#include <vector>

struct BrowserEntry {
    String name;
    bool dir = false;
};

struct BrowserState {
    bool open = true;
    String path;
    std::vector<BrowserEntry> entries;
    int sel = 0;
    int top = 0;
    bool truncated = false;
};

namespace browser {

void begin();
const BrowserState& state();
void refresh();
void open(bool open);
void freeEntries();
void move(int d);
void page(int d);
void activate();
void upDir();

}
