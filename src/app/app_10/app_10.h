/**
 * @file app_10.h
 * @brief I2C Explorer — scan the shared internal/expansion I2C bus.
 */
#pragma once
#include <mooncake.h>
#include "../../bsp/devices.h"
#include "../../bsp/i2c/I2C_Class.hpp"
#include "../app_common/app_ui.h"
#include <cstdint>

using namespace mooncake;

namespace MOONCAKE::APPS
{
    class App10 : public AppAbility {
    public:
        App10(DEVICES* device);
        void onOpen() override;
        void onRunning() override;
        void onClose() override;

    private:
        DEVICES* _device = nullptr;
        AppUI::ListMenu _menu;

        bool    _menuReady = false;
        uint8_t _scanAddr  = 0x08;
        bool    _found[128]{};
        int     _foundCount = 0;

        void _startScan();
        void _scanStep();
        void _finishScan();
        void _drawScanScreen() const;

        static const char* _knownDeviceName(uint8_t address);
    };
}
