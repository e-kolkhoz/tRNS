#pragma once
#include <USBMSC.h>
#include "wear_levelling.h"

// USB MSC: ffat-партиция как флешка на ПК.
// Вызывать mount() из setup() после задержки (даём OTG отойти после TinyUF2).
class USBFlash {
public:
    static bool mount();
    static bool isMounted();

private:
    static USBMSC      _msc;
    static wl_handle_t _wl;
    static bool        _mounted;
    static uint32_t    _sector_count;
    static uint32_t    _sector_size;

    static int32_t _onRead(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize);
    static int32_t _onWrite(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize);
};
