/**
 * @file app_11_hitachi.cpp
 * @brief App11 AC Remote - direct driver for the IRHitachiAc424 family.
 *        See app_11_hitachi.h for why the generic IRac path is bypassed here.
 */
#include "app_11_hitachi.h"

#include <IRac.h>

namespace achitachi {

namespace {

/* The frame's Button byte for each pad. The 344 and 264 aliases are defined
 * equal to the 424 values in ir_Hitachi.h, so one table covers all three. */
uint8_t buttonCode(Key key)
{
    switch (key) {
        case Key::TempUp:   return kHitachiAc424ButtonTempUp;     /* 0x44 */
        case Key::TempDown: return kHitachiAc424ButtonTempDown;   /* 0x43 */
        case Key::Fan:      return kHitachiAc424ButtonFan;        /* 0x42 */
        case Key::SwingV:   return kHitachiAc424ButtonSwingV;     /* 0x81 */
        case Key::SwingH:   return kHitachiAc424ButtonSwingH;     /* 0x8C */
        default:            return kHitachiAc424ButtonPowerMode;  /* 0x13 */
    }
}

}  // namespace

bool isSupported(decode_type_t protocol)
{
    switch (protocol) {
#if SEND_HITACHI_AC424
        case decode_type_t::HITACHI_AC424:
#endif
#if SEND_HITACHI_AC344
        case decode_type_t::HITACHI_AC344:
#endif
#if SEND_HITACHI_AC264
        case decode_type_t::HITACHI_AC264:
#endif
            return IRac::isProtocolSupported(protocol);
        default:
            return false;
    }
}

stdAc::opmode_t effectiveMode(stdAc::opmode_t mode)
{
    /* Mirrors IRHitachiAc424::convertMode(): only Cool/Heat/Dry/Fan exist,
     * everything else (Auto, Off) is encoded as Cool. */
    switch (mode) {
        case stdAc::opmode_t::kHeat:
        case stdAc::opmode_t::kDry:
        case stdAc::opmode_t::kFan:
            return mode;
        default:
            return stdAc::opmode_t::kCool;
    }
}

Direct::~Direct() { close(); }

void Direct::close()
{
    /* Deleted through the concrete pointer: IRHitachiAc424 has no virtual
     * destructor, so deleting a subclass through the base would be undefined. */
    if (_ac344) { delete _ac344; _ac344 = nullptr; }
    if (_ac264) { delete _ac264; _ac264 = nullptr; }
    if (_ac424) { delete _ac424; _ac424 = nullptr; }
    _base = nullptr;
}

bool Direct::open(decode_type_t protocol, uint16_t pin, const stdAc::state_t& seed)
{
    close();
    if (!isSupported(protocol)) return false;

    switch (protocol) {
#if SEND_HITACHI_AC344
        case decode_type_t::HITACHI_AC344:
            _ac344 = new IRHitachiAc344(pin);
            _base  = _ac344;
            break;
#endif
#if SEND_HITACHI_AC264
        case decode_type_t::HITACHI_AC264:
            _ac264 = new IRHitachiAc264(pin);
            _base  = _ac264;
            break;
#endif
#if SEND_HITACHI_AC424
        case decode_type_t::HITACHI_AC424:
            _ac424 = new IRHitachiAc424(pin);
            _base  = _ac424;
            break;
#endif
        default:
            return false;
    }
    if (!_base) return false;

    _base->begin();
    /* Seed the object with the stored state so the first press is a delta from
     * what the machine was last told - _previoustemp included - and send
     * nothing while doing it. */
    _apply(seed);
    _base->setButton(kHitachiAc424ButtonPowerMode);
    return true;
}

/* Applies every field. Each of these setters writes the Button byte as a side
 * effect (setMode/setPower -> 0x13, setTemp -> 0x43/0x44, setFan -> 0x42 when
 * the speed changed, setSwingV -> 0x81, setSwingH -> 0x8C), which is exactly
 * why send() stamps the real key code after this returns. */
void Direct::_apply(const stdAc::state_t& s)
{
    if (!_base) return;

    _base->setMode(IRHitachiAc424::convertMode(effectiveMode(s.mode)));
    _base->setTemp((uint8_t)s.degrees);
    _base->setFan(_base->convertFan(s.fanspeed));

    const bool swingOn = (s.swingv != stdAc::swingv_t::kOff);
    if (_ac344) {
        _ac344->setSwingV(swingOn);
        _ac344->setSwingH(IRHitachiAc344::convertSwingH(s.swingh));
    } else {
        _base->setSwingVToggle(swingOn);
    }

    _base->setPower(s.power);
}

bool Direct::send(const stdAc::state_t& state, Key key)
{
    if (!_base) return false;

    _apply(state);
    /* LAST write before the frame goes out. */
    _base->setButton(buttonCode(key));

    if (_ac344)      _ac344->send();
    else if (_ac264) _ac264->send();
    else             _ac424->send();
    return true;
}

}  // namespace achitachi
