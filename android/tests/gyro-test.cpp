// Host-only unit test of Android gyro integration; no Windows game build or SDL runtime needed.
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <limits>
#define __ANDROID__ 1
#include "core/gyro.cpp"

namespace logx { void Write(Level, const char*, ...) {} }
static Uint64 ticks = 1'000'000'000;
extern "C" {
Uint64 SDL_GetTicksNS() { return ticks; }
SDL_InitFlags SDL_WasInit(SDL_InitFlags) { return 0; }
bool SDL_AddEventWatch(SDL_EventFilter, void*) { return true; }
SDL_JoystickID* SDL_GetGamepads(int*) { return nullptr; }
SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID) { return nullptr; }
bool SDL_GamepadHasSensor(SDL_Gamepad*, SDL_SensorType) { return false; }
bool SDL_GamepadSensorEnabled(SDL_Gamepad*, SDL_SensorType) { return false; }
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad*, SDL_SensorType, bool) { return true; }
const char* SDL_GetGamepadName(SDL_Gamepad*) { return "test"; }
void SDL_free(void*) {}
}

void Near(float a, float b) { assert(std::fabs(a - b) < 0.001f); }

int main() {
    using namespace gyro;
    Settings s;
    assert(!s.enabled && s.source == kPhone);
    s.enabled = true;
    s.source = kPhone;
    s.sensitivity = 1;
    Take(s);

    // All four Surface rotations produce the same screen-right pitch and screen-up yaw.
    for (int r = 0; r < 4; ++r) {
        ResetPhone(true);
        auto sample = [&](bool accel, float x, float y, float z, uint64_t ns) {
            float nx = x, ny = y;
            if (r == 1) { nx = y; ny = -x; }
            if (r == 2) { nx = -x; ny = -y; }
            if (r == 3) { nx = -y; ny = x; }
            PhoneSample(accel, nx, ny, z, ns, r);
        };
        sample(true, 0, 9.81f, 0, ticks);
        sample(false, 1, 1, 0, ticks);
        sample(false, 1, 1, 0, ticks + 10'000'000);
        ticks += 10'000'000;
        Turn t = Take(s);
        Near(t.yaw, kDegreesPerRadian * 0.01f);
        Near(t.pitch, -kDegreesPerRadian * 0.01f);
        Near(Take(s).yaw, 0); // samples consumed once
        // Duplicate, old and invalid samples cannot inject motion or rewind time.
        sample(false, 100, 100, 0, ticks);
        sample(false, 100, 100, 0, ticks - 1);
        sample(false, std::numeric_limits<float>::quiet_NaN(), 1, 0, ticks + 1);
        Near(Take(s).yaw, 0);
    }

    // Each gyro inversion is independent, for every yaw mode and both sensor sources (shared Take path).
    for (uint32_t axis : {kYaw, kRoll, kWorld}) {
        s.axis = axis;
        for (int mask = 0; mask < 4; ++mask) {
            s.invert_x = mask & 1;
            s.invert_y = mask & 2;
            ResetPhone(true);
            PhoneSample(true, 0, 9.81f, 0, ticks, 0);
            PhoneSample(false, 1, 1, 1, ticks, 0);
            PhoneSample(false, 1, 1, 1, ticks + 10'000'000, 0);
            ticks += 10'000'000;
            Turn t = Take(s);
            Near(t.yaw, kDegreesPerRadian * 0.01f * (s.invert_x ? -1 : 1));
            Near(t.pitch, kDegreesPerRadian * 0.01f * (s.invert_y ? 1 : -1));
        }
    }
    s.invert_x = s.invert_y = false;
    s.axis = kYaw;

    ResetPhone(true);
    PhoneSample(true, 0, 0, 9.81f, ticks, 0);
    PhoneSample(false, 0, 0, 1, ticks, 0);
    PhoneSample(false, 0, 0, 1, ticks + 10'000'000, 0);
    ticks += 10'000'000;
    s.axis = kWorld;
    Near(Take(s).yaw, kDegreesPerRadian * 0.01f); // tilted phone: world-up projection
    s.axis = kRoll;
    PhoneSample(false, 0, 0, 1, ticks + 10'000'000, 0);
    ticks += 10'000'000;
    Near(Take(s).yaw, kDegreesPerRadian * 0.01f);
    s.axis = kYaw;

    ResetPhone(false);
    PhoneSample(false, 0, 100, 0, ticks + 10'000'000, 0);
    Near(Take(s).yaw, 0); // background readings ignored
    ResetPhone(true);
    PhoneSample(false, 0, 1, 0, ticks + 1'000'000'000, 0);
    Near(Take(s).yaw, 0); // first reading after resume never integrates the pause
    ticks += 1'000'000'000;
    Take(s);
    PhoneSample(false, 0, 1, 0, ticks + 10'000'000, 0);
    PhoneSample(false, 1, 0, 0, ticks + 20'000'000, 1);
    Near(Take(s).yaw, 0); // rotation discards old turns and resets integration

    ResetPhone(true);
    PhoneSample(false, 0, 1, 0, ticks, 0);
    PhoneSample(false, 0, 1, 0, ticks + 500'000'000, 0);
    Near(Take(s).yaw, 0); // sample stall
    PhoneSample(false, 0, 1, 0, ticks + 510'000'000, 0);
    ticks += 1'000'000'000;
    Near(Take(s).yaw, 0); // stale camera update drops accumulated turn

    ResetPhone(true);
    PhoneSample(false, 0, 0.02f, 0, ticks, 0);
    StartCalibration(kPhone);
    assert(Calibrating(kPhone));
    for (int i = 0; i < 210; ++i) {
        ticks += 10'000'000;
        PhoneSample(true, 0, 9.81f, 0, ticks, 0);
        PhoneSample(false, 0, 0.02f, 0, ticks, 0);
        Near(Take(s).yaw, 0);
    }
    assert(!Calibrating(kPhone));
    assert(std::string(CalibrationStatus(kPhone)) == "DONE");
    Near(g_phone.drift.y, 0.02f * kDegreesPerRadian);

    // Phone noise that exceeded the controller accelerometer threshold can still calibrate.
    StartCalibration(kPhone);
    for (int i = 0; i < 210; ++i) {
        ticks += 10'000'000;
        PhoneSample(true, (i % 2 ? 0.1f : -0.1f), 9.81f, 0, ticks, 0);
        PhoneSample(false, 0, 0.02f, 0, ticks, 0);
        Take(s);
    }
    assert(std::string(CalibrationStatus(kPhone)) == "DONE");
    StartCalibration(kPhone);
    for (int i = 0; i < 1100; ++i) {
        ticks += 10'000'000;
        PhoneSample(true, (i % 2 ? 2.0f : -2.0f), 9.81f, 0, ticks, 0);
        PhoneSample(false, 0, (i % 2 ? 1.0f : -1.0f), 0, ticks, 0);
        Take(s);
    }
    assert(std::string(CalibrationStatus(kPhone)) == "RETRY");
    StartCalibration(kPhone);
    ticks += 11'000'000'000;
    assert(std::string(CalibrationStatus(kPhone)) == "RETRY"); // sensor stops reporting
    ResetPhone(true);
    StartCalibration(kPhone);
    assert(std::string(CalibrationStatus(kPhone)) == "RETRY"); // no sensor yet

    // Phone ignores saved controller-only gates; switching sources/off drops pending input.
    s.button = 1;
    s.button_mode = kOnWhileHeld;
    Take(s); // clear the deliberately stale camera interval from the timeout test
    ResetPhone(true);
    PhoneSample(false, 0, 1, 0, ticks, 0);
    PhoneSample(false, 0, 1, 0, ticks + 10'000'000, 0);
    ticks += 10'000'000;
    assert(Take(s).yaw > 0);
    s.source = kController;
    Near(Take(s).yaw, 0);
    SDL_GamepadSensorEvent event{};
    event.which = 123;
    event.sensor = SDL_SENSOR_GYRO;
    event.data[1] = 1;
    event.sensor_timestamp = ticks;
    ReadSensor(g_motion, event);
    event.sensor_timestamp += 10'000'000;
    ReadSensor(g_motion, event);
    ticks += 10'000'000;
    Near(Take(s).yaw, 0); // controller gate still applies
    s.button = 0;
    event.sensor_timestamp += 10'000'000;
    ReadSensor(g_motion, event);
    ticks += 10'000'000;
    assert(Take(s).yaw > 0);
    s.source = kPhone;
    Near(Take(s).yaw, 0);
    s.enabled = false;
    Near(Take(s).yaw, 0);
    s.enabled = true;
    Near(Take(s).yaw, 0);
}
