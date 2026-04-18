#include "usb_flash.h"
#include "storage_control.h"
#include <Arduino.h>
#include <USB.h>
#include <esp_partition.h>
#include <esp_system.h>

USBMSC      USBFlash::_msc;
wl_handle_t USBFlash::_wl           = WL_INVALID_HANDLE;
bool        USBFlash::_mounted      = false;
uint32_t    USBFlash::_sector_count = 0;
uint32_t    USBFlash::_sector_size  = 0;

bool USBFlash::mount() {
    if (_mounted) return true;

    // FFat и MSC не могут одновременно держать одну партицию.
    StorageControl::end();

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
    if (!part) return false;

    if (wl_mount(part, &_wl) != ESP_OK) return false;

    _sector_size  = (uint32_t)wl_sector_size(_wl);
    _sector_count = (uint32_t)(wl_size(_wl) / _sector_size);

    _msc.vendorID("ESP32");
    _msc.productID("tRNS-Data");
    _msc.productRevision("1.00");
    _msc.onRead(_onRead);
    _msc.onWrite(_onWrite);
    _msc.mediaPresent(true);
    _msc.begin(_sector_count, (uint16_t)_sector_size);

    USB.begin();

    _mounted = true;
    return true;
}

bool USBFlash::isMounted() {
    return _mounted;
}

int32_t USBFlash::_onRead(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
    if (_wl == WL_INVALID_HANDLE) return -1;
    esp_err_t err = wl_read(_wl, (size_t)lba * _sector_size + offset, buf, bufsize);
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

int32_t USBFlash::_onWrite(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
    if (_wl == WL_INVALID_HANDLE) return -1;

    size_t sector_addr = (size_t)lba * _sector_size;

    if (offset == 0 && bufsize == _sector_size) {
        wl_erase_range(_wl, sector_addr, _sector_size);
        wl_write(_wl, sector_addr, buf, _sector_size);
    } else {
        uint8_t* tmp = (uint8_t*)heap_caps_malloc(_sector_size, MALLOC_CAP_8BIT);
        if (!tmp) return -1;
        wl_read(_wl, sector_addr, tmp, _sector_size);
        memcpy(tmp + offset, buf, bufsize);
        wl_erase_range(_wl, sector_addr, _sector_size);
        wl_write(_wl, sector_addr, tmp, _sector_size);
        heap_caps_free(tmp);
    }

    return (int32_t)bufsize;
}
