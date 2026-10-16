#include "browser.h"
#include "config.h"
#include "player.h"
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>

namespace {

BrowserState s_bs;
constexpr const char* ROOT = "/sd";

int naturalCmp(const String& a, const String& b) {
    const char* pa = a.c_str();
    const char* pb = b.c_str();
    while (*pa && *pb) {
        if (isdigit((unsigned char)*pa) && isdigit((unsigned char)*pb)) {
            char* ea;
            char* eb;
            long na = strtol(pa, &ea, 10);
            long nb = strtol(pb, &eb, 10);
            if (na != nb) return na < nb ? -1 : 1;
            pa = ea;
            pb = eb;
        } else {
            int ca = tolower((unsigned char)*pa);
            int cb = tolower((unsigned char)*pb);
            if (ca != cb) return ca < cb ? -1 : 1;
            pa++;
            pb++;
        }
    }
    if (*pa) return 1;
    if (*pb) return -1;
    return 0;
}

void keepSelectionVisible() {
    int n = (int)s_bs.entries.size();
    if (s_bs.sel < 0) s_bs.sel = 0;
    if (s_bs.sel > n - 1) s_bs.sel = n > 0 ? n - 1 : 0;
    if (s_bs.sel < s_bs.top) s_bs.top = s_bs.sel;
    if (s_bs.sel >= s_bs.top + cfg::BROWSER_ROWS) s_bs.top = s_bs.sel - cfg::BROWSER_ROWS + 1;
    if (s_bs.top > n - cfg::BROWSER_ROWS) s_bs.top = n - cfg::BROWSER_ROWS;
    if (s_bs.top < 0) s_bs.top = 0;
}

}

namespace browser {

void refresh() {
    s_bs.entries.clear();
    s_bs.truncated = false;
    DIR* d = opendir(s_bs.path.c_str());
    if (!d) {
        s_bs.path = ROOT;
        d = opendir(s_bs.path.c_str());
        if (!d) return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;
        String name(n);
        if (name == "System Volume Information") continue;
        bool isDir = false;
        if (e->d_type == DT_DIR) {
            isDir = true;
        } else if (e->d_type == DT_UNKNOWN) {
            String full = s_bs.path + "/" + name;
            struct stat stt;
            if (stat(full.c_str(), &stt) == 0 && S_ISDIR(stt.st_mode)) isDir = true;
        }
        if (!isDir && !player::isPlayable(name)) continue;
        s_bs.entries.push_back({name, isDir});
        if ((int)s_bs.entries.size() >= cfg::MAX_BROWSER_ENTRIES) {
            s_bs.truncated = true;
            break;
        }
    }
    closedir(d);

    std::sort(s_bs.entries.begin(), s_bs.entries.end(),
              [](const BrowserEntry& x, const BrowserEntry& y) {
                  if (x.dir != y.dir) return x.dir;
                  return naturalCmp(x.name, y.name) < 0;
              });
    s_bs.sel = 0;
    s_bs.top = 0;
}

void begin() {
    s_bs.path = player::savedDir();
    s_bs.open = true;
    refresh();
}

const BrowserState& state() { return s_bs; }

void open(bool o) { s_bs.open = o; }

void freeEntries() {
    std::vector<BrowserEntry>().swap(s_bs.entries);
    s_bs.sel = 0;
    s_bs.top = 0;
}

void move(int d) {
    if (s_bs.entries.empty()) return;
    s_bs.sel += d;
    keepSelectionVisible();
}

void page(int d) {
    if (s_bs.entries.empty()) return;
    s_bs.sel += d * cfg::BROWSER_ROWS;
    keepSelectionVisible();
}

void activate() {
    if (s_bs.entries.empty()) return;
    const BrowserEntry& e = s_bs.entries[s_bs.sel];
    if (e.dir) {
        s_bs.path += "/" + e.name;
        refresh();
        return;
    }
    std::vector<String> items;
    int idx = 0;
    for (int i = 0; i < (int)s_bs.entries.size(); i++) {
        if (s_bs.entries[i].dir) continue;
        if (i == s_bs.sel) idx = (int)items.size();
        items.push_back(s_bs.path + "/" + s_bs.entries[i].name);
    }
    if (items.empty()) return;
    player::saveDir(s_bs.path);
    player::playList(items, idx);
    s_bs.open = false;
}

void upDir() {
    if (s_bs.path == ROOT || s_bs.path.length() <= 3) {
        s_bs.open = false;
        return;
    }
    int i = s_bs.path.lastIndexOf('/');
    s_bs.path = i > 0 ? s_bs.path.substring(0, i) : String(ROOT);
    refresh();
}

}
