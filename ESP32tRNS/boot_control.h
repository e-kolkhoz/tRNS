#pragma once
#include <driver/rtc_io.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// Управление режимами загрузки.
// Основной сценарий: esp_ota_set_boot_partition("uf2") + esp_restart()
// Fallback:          RTC GPIO hold на GPIO0 через перезагрузку
class BootControl {
public:
    // Вызывать первым в setup(): снимает GPIO hold, если был выставлен при прошлом rebootToUF2()
    static void init();

    // Перезагрузка в UF2 режим (плата появляется как диск WS3ZEROBOOT)
    static void rebootToUF2();

    // Лог текущего/загрузочного раздела в Serial
    static void logPartitionInfo();

private:
    static constexpr gpio_num_t UF2_GPIO = GPIO_NUM_0;
    static void _rebootViaGPIOHold();
};
