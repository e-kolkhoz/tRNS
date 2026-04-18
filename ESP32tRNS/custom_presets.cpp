#include "custom_presets.h"
#include <FFat.h>

std::vector<PresetInfo> CustomPresets::checkAll(uint32_t expectedRate) {
    std::vector<PresetInfo> out;

    if (!FFat.begin(true, "/ffat", 10, "ffat")) return out;

    File root = FFat.open("/");
    if (root && root.isDirectory()) {
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            if (f.isDirectory()) continue;
            String path = String("/") + f.name();
            f.close();

            WavInfo info;
            if (WavReader::probe(path.c_str(), &info, expectedRate)) {
                out.push_back({ path, info });
            }
        }
    }

    FFat.end();
    return out;
}
