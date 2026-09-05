/**
 * @file device_status.cpp
 * @brief Device state aggregator — polls hardware and fills DeviceStatus.
 *
 * TODO fields marked below need companion BSP/service wired in:
 *   - wifi_*    : WiFi_Class (bsp/wifi)
 *   - bt_active : BLE profile state (no BSP wrapper yet)
 *   - sd_present: SDMMC_Class or active probe
 */
#include "device_status.h"
#include "settings_bridge.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include "usb_msc.h"

static DeviceStatus s_status = {};

void device_status_update(DEVICES* dev)
{
    /* ── WiFi ────────────────────────────────────────────────── */
    s_status.wifi_connected = (WiFi.status() == WL_CONNECTED);
    if (s_status.wifi_connected) {
        strncpy(s_status.wifi_ssid, WiFi.SSID().c_str(), sizeof(s_status.wifi_ssid) - 1);
        strncpy(s_status.wifi_ip,   WiFi.localIP().toString().c_str(), sizeof(s_status.wifi_ip) - 1);
    } else {
        s_status.wifi_ssid[0] = '\0';
        s_status.wifi_ip[0]   = '\0';
    }

    /* ── BT/BLE ──────────────────────────────────────────────── */
    /* TODO: query BLE_Class / BluetoothA2DP state when available */
    /* s_status.bt_active = ble.isConnected() || bt_audio.isConnected(); */

    /* ── Brightness / Volume from settings bridge ────────────── */
    s_status.lcd_brightness = sys_get_brightness();

    /* ── SD card — active probe (hot-plug safe) ──────────────── */
    /* While USB MSC owns the card the FAT volume is unmounted, so the probe
     * would report "removed" and the launcher would run its SD-removed flow
     * in the middle of a host transfer. Keep the last known value instead. */
    if (!usb_msc_is_active()) {
        File f = SD_MMC.open("/");
        s_status.sd_present = (bool)f;
        if (f) f.close();
    }

    /* ── Battery / charging from AXP173 ─────────────────────── */
    if (dev) {
        float lvl = dev->pmu.getBatLevel();
        s_status.battery_pct = (int)constrain(lvl, 0.0f, 100.0f);
        /* AXP173: charging when VBUS present and charger active */
        s_status.charging = dev->pmu.isVBUSExist() && dev->pmu.isCharging();
    }
}

const DeviceStatus* device_status_get(void)
{
    return &s_status;
}

void device_status_set_btn_sound(bool on)
{
    s_status.btn_sound = on;
}

void device_status_set_led_brightness(int pct)
{
    s_status.led_brightness = (pct < 0) ? 0 : (pct > 100) ? 100 : pct;
}
