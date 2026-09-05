/**
 * @file app_11_hitachi.h
 * @brief App11 AC Remote - direct driver for the IRHitachiAc424 family.
 *
 * Why this exists (docs/app11-ac-remote.md section 5, hardware 2026-09-06):
 * a Hitachi 424/344/264 frame carries a "Button" byte naming the key that was
 * pressed, and the indoor unit only applies the field that key belongs to.
 * `IRac::hitachi344()` ends with `setPower()`, which stamps the Button byte
 * with power/mode on every frame, so the owner's RAS-22NK accepted power and
 * mode changes but never moved off 24 C. IRac also builds a fresh protocol
 * object per send, which resets the class's `_previoustemp`.
 *
 * So for these three protocols the app keeps ONE protocol object alive for as
 * long as a device is open, applies the fields through the class setters, and
 * stamps the key code LAST - the library's own setters write the Button byte
 * as a side effect, so nothing but setButton() may follow it.
 *
 * Every other protocol keeps the generic IRac path.
 */
#pragma once
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Hitachi.h>

namespace achitachi {

/* Which pad was pressed. Maps to the frame's Button byte. */
enum class Key : uint8_t { PowerMode, TempUp, TempDown, Fan, SwingV, SwingH };

/* True for the three IRHitachiAc424-derived protocols this build can send. */
bool isSupported(decode_type_t protocol);

/* The library's 424 family has no Auto mode: convertMode() folds it into
 * Cool. The app asks first so the panel never promises a mode the frame does
 * not carry. */
stdAc::opmode_t effectiveMode(stdAc::opmode_t mode);

/* One live protocol object for the device currently open. The class holds the
 * raw frame and _previoustemp between presses - exactly the state a physical
 * remote keeps - so it must outlive a single send. */
class Direct {
 public:
    Direct() = default;
    ~Direct();
    Direct(const Direct&) = delete;
    Direct& operator=(const Direct&) = delete;

    /* Create the object for `protocol` and seed it from `seed` WITHOUT
     * transmitting. Returns false (and stays closed) for anything else. */
    bool open(decode_type_t protocol, uint16_t pin, const stdAc::state_t& seed);
    void close();
    bool isOpen() const { return _base != nullptr; }

    /* Apply `state`, stamp `key` last, transmit. false = not open. */
    bool send(const stdAc::state_t& state, Key key);

 private:
    void _apply(const stdAc::state_t& state);

    /* The base class has no virtual destructor, so each concrete type is
     * deleted through its own pointer; `_base` is only ever borrowed. */
    IRHitachiAc424* _base  = nullptr;
    IRHitachiAc424* _ac424 = nullptr;
    IRHitachiAc344* _ac344 = nullptr;
    IRHitachiAc264* _ac264 = nullptr;
};

}  // namespace achitachi
