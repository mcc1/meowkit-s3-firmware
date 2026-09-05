/**
 * @file app_10.cpp
 * @brief I2C Explorer — non-destructive shared-bus address scanner.
 */
#include "app_10.h"
#include <Arduino.h>
#include <cstring>

namespace {
    struct KnownI2CDevice {
        uint8_t     address;
        const char* name;
    };

    /* Names come from the firmware BSP address table.  A hit is still only an
     * address-level response; it does not prove the exact chip identity. */
    constexpr KnownI2CDevice kKnownDevices[] = {
        { 0x18, "ES8311 audio codec" },
        { 0x19, "PCA9557 IO expander" },
        { 0x34, "AXP173 PMU"          },
        { 0x38, "FT6336 touch"        },
        { 0x41, "ES7210 audio ADC"    },
        { 0x44, "SHT30 temperature"  },
        { 0x51, "PCF8563 RTC"        },
        { 0x68, "BMI270 IMU"          },
    };
}

namespace MOONCAKE::APPS
{
    App10::App10(DEVICES* device)
        : _device(device)
    {
        setAppInfo().name = "I2C Explorer";
    }

    void App10::onOpen()
    {
        _menu.init(&_device->Lcd, &_device->button, "I2C Explorer");
        _menu.setOnSelect([this](int, const AppUI::MenuItem&) {
            _startScan();
        });
        _menu.setOnBack([this]() {
            close();
        });

        Serial.println("[App10] I2C Explorer opened");
        _startScan();
    }

    void App10::onRunning()
    {
        _device->button.update();
        _device->button.tick();

        /* Long-press B remains a universal app exit path. */
        if (_device->button.B.isLongPress()) {
            close();
            return;
        }

        if (!_menuReady) {
            _scanStep();
            return;
        }

        _menu.update();
    }

    void App10::onClose()
    {
        _menuReady = false;
        _device->Lcd.fillScreen(TFT_BLACK);
        Serial.println("[App10] I2C Explorer closed");
    }

    void App10::_startScan()
    {
        _menuReady  = false;
        _scanAddr   = 0x08;
        _foundCount = 0;
        memset(_found, 0, sizeof(_found));
        _menu.clear();

        _device->Lcd.fillScreen(TFT_BLACK);
        _drawScanScreen();
        Serial.println("I2C_SCAN_BEGIN,bus=In_I2C,sda=1,scl=2,freq=100000,range=0x08-0x77");
    }

    void App10::_scanStep()
    {
        /* Four addresses per frame keeps the scan responsive while the
         * driver's 20 ms NACK timeout prevents a missing device from stalling
         * the whole scan in one long blocking call. */
        if (!In_I2C.isEnabled()) {
            _scanAddr = 0x78;
            _drawScanScreen();
            _finishScan();
            return;
        }

        for (int i = 0; i < 4 && _scanAddr < 0x78; ++i, ++_scanAddr) {
            if (In_I2C.scanID(_scanAddr)) {
                _found[_scanAddr] = true;
                ++_foundCount;
                Serial.printf("I2C_SCAN_ACK,address=0x%02X,name=%s\n",
                              _scanAddr, _knownDeviceName(_scanAddr));
            }
        }

        _drawScanScreen();
        if (_scanAddr >= 0x78) {
            _finishScan();
        }
    }

    void App10::_finishScan()
    {
        std::vector<AppUI::MenuItem> items;
        items.reserve((size_t)_foundCount);

        for (int address = 0; address < 128; ++address) {
            if (!_found[address]) continue;

            char addressText[12];
            snprintf(addressText, sizeof(addressText), "0x%02X", address);
            items.push_back({addressText, _knownDeviceName((uint8_t)address), address});
        }

        _menu.setItems(items);

        char status[48];
        if (!In_I2C.isEnabled()) {
            snprintf(status, sizeof(status), "I2C bus unavailable");
        } else if (_foundCount == 0) {
            snprintf(status, sizeof(status), "No ACK  |  A rescan  B back");
        } else {
            snprintf(status, sizeof(status), "%d found  |  A rescan  B back", _foundCount);
        }
        _menu.setStatus(status);
        _menuReady = true;

        Serial.printf("I2C_SCAN_END,count=%d\n", _foundCount);
    }

    void App10::_drawScanScreen() const
    {
        auto& lcd = _device->Lcd;
        const int total = 0x78 - 0x08;
        const int done  = (_scanAddr >= 0x78) ? total : ((int)_scanAddr - 0x08);
        const int barW  = 280;
        const int barX  = 20;

        lcd.setTextDatum(textdatum_t::top_left);
        lcd.setFont(&fonts::FreeSansBold9pt7b);
        lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        lcd.fillScreen(TFT_BLACK);
        lcd.drawString("I2C Explorer", 12, 10);

        lcd.setFont(&fonts::Font0);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.drawString("Shared bus: SDA 1 / SCL 2", 12, 42);
        lcd.drawString("100 kHz  |  0x08 - 0x77", 12, 58);

        lcd.drawRect(barX, 91, barW, 16, TFT_WHITE);
        const int fillW = (done * (barW - 4)) / total;
        if (fillW > 0) lcd.fillRect(barX + 2, 93, fillW, 12, TFT_GREEN);

        char progress[32];
        snprintf(progress, sizeof(progress), "Scanning 0x%02X (%d/%d)",
                 (_scanAddr >= 0x78) ? 0x77 : _scanAddr, done, total);
        lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        lcd.drawString(progress, 12, 125);
        lcd.setTextColor(0x7BEF, TFT_BLACK);
        lcd.drawString("B: back", 12, 205);
    }

    const char* App10::_knownDeviceName(uint8_t address)
    {
        for (const auto& device : kKnownDevices) {
            if (device.address == address) return device.name;
        }
        return "Unknown / external device";
    }
}
