// Gyro aiming: the controller's gyro turns the view like a mouse, beside the right stick.
// SDL's thread reads every reading as it arrives; the game takes the turn gathered since its last look update.
#include "core/gyro.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <mutex>

#include <SDL3/SDL.h>

#include "core/log.h"

namespace gyro {

namespace {

constexpr float kDegreesPerRadian = 57.2957795f;
constexpr float kSecondsPerNs = 1e-9f;
constexpr float kMaxSampleGap = 0.05f;  // seconds; a longer gap between readings is a stall, not motion
constexpr uint64_t kStaleNs = 100'000'000;  // turn gathered while the look update did not run is dropped
constexpr uint64_t kScanNs = 1'000'000'000;  // controllers plugged in since get their sensors on
constexpr float kGravitySeconds = 0.25f;  // the accelerometer's smoothing into the controller's up
// a controller at rest stays within these through a whole window; the gyro's mean there is its drift.
// A slow steady turn passes the spread tests too, so only a mean this small is taken for drift.
constexpr float kRestSeconds = 2.0f;
constexpr float kRestGyroSpread = 1.0f;  // degrees a second
constexpr float kRestAccelSpread = 0.1f;  // metres a second squared
constexpr float kMaxDrift = 1.5f;  // degrees a second
// turns slower than this are smoothed over kSmoothSeconds, twice as fast pass as they are
constexpr float kSmoothSpeed = 5.0f;  // degrees a second
constexpr float kSmoothSeconds = 0.1f;

// kButtonNames in order
constexpr SDL_GamepadButton kButtons[] = {
    SDL_GAMEPAD_BUTTON_INVALID,       SDL_GAMEPAD_BUTTON_TOUCHPAD,      SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2};
static_assert(std::size(kButtons) == std::size(kButtonNames));
static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32);

// SDL's axes: x right, y up out of the face, z toward the player
struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
float Largest(const Vec3& a) { return std::max({std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}); }

// the lowest and highest reading on each axis
struct Spread {
    Vec3 low;
    Vec3 high;
    bool empty = true;

    void Add(const Vec3& v) {
        if (empty) {
            low = v;
            high = v;
            empty = false;
            return;
        }
        low = {std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z)};
        high = {std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z)};
    }
    float Widest() const { return empty ? 0.0f : Largest(Sub(high, low)); }
};

// the gyro's mean over a window
struct Average {
    Vec3 sum;
    int count = 0;
    float seconds = 0;

    void Push(const Vec3& v, float dt) {
        sum = Add(sum, v);
        count++;
        seconds += dt;
    }
    Vec3 Mean() const { return count ? Scale(sum, 1.0f / float(count)) : Vec3{}; }
};

// the window the controller is tested for rest through
struct RestWindow {
    Spread gyro;
    Spread accel;
    Average mean;
};

// the controller aiming: the first whose gyro reports, until it is unplugged
struct Motion {
    SDL_JoystickID pad = 0;
    uint64_t gyro_ns = 0;  // sensor clock of the last readings
    uint64_t accel_ns = 0;
    Vec3 gravity;  // smoothed accelerometer, pointing up
    Vec3 drift;  // degrees a second the gyro reads at rest
    Vec3 smoothed;  // the slow share of the turn, degrees a second
    RestWindow rest;
    bool calibrating = false;  // the next still window sets the drift, however large
    bool calibrated = false;  // by hand: rest windows stop, a slow steady turn would pass for drift
    uint64_t calibration_deadline = 0;
    bool calibration_failed = false;
    // degrees turned since the last take: about the controller's up, its front, the world's up; pitch down positive
    float yaw = 0;
    float roll = 0;
    float world_yaw = 0;
    float pitch = 0;
    uint32_t held = 0;  // bits by SDL button
    uint32_t pressed = 0;  // went down since the last take
};

std::mutex g_lock;
Motion g_motion;
#ifdef __ANDROID__
Motion g_phone;
bool g_phone_active = false;
int g_phone_rotation = -1;
#endif
bool g_toggled_off = false;  // the gyro button in kToggle mode

// seconds since the last reading, 0 for the first and for a stall
float Gap(uint64_t& last, uint64_t now) {
    int64_t ns = last ? int64_t(now - last) : 0;
    last = now;
    float seconds = float(ns) * kSecondsPerNs;
    return seconds > 0 && seconds <= kMaxSampleGap ? seconds : 0.0f;
}

Vec3 Up(const Motion& m) {
    float length = Length(m.gravity);
    if (length < 1.0f) return {0, 1, 0};  // no accelerometer yet
    return Scale(m.gravity, 1.0f / length);
}

void ReadAccel(Motion& m, const Vec3& accel, uint64_t ns) {
    bool first = !m.accel_ns;
    float dt = Gap(m.accel_ns, ns);
    float k = first ? 1.0f : dt / (kGravitySeconds + dt);
    m.gravity = Add(m.gravity, Scale(Sub(accel, m.gravity), k));
    m.rest.accel.Add(accel);
}

// a window the controller lay still through sets the drift to the gyro's mean over it
void UpdateDrift(Motion& m, const Vec3& gyro, float dt, bool phone) {
    RestWindow& w = m.rest;
    w.gyro.Add(gyro);
    w.mean.Push(gyro, dt);
    if (w.mean.seconds < kRestSeconds) return;
    Vec3 mean = w.mean.Mean();
    // Handheld phone accelerometers are noisier than controller sensors. Only manual phone calibration
    // uses the wider rest window; automatic drift correction keeps the conservative controller limits.
    bool manual_phone = phone && m.calibrating;
    bool still = w.gyro.Widest() < (manual_phone ? 3.0f : kRestGyroSpread)
        && w.accel.Widest() < (manual_phone ? 0.5f : kRestAccelSpread)
        && (!manual_phone || Largest(mean) < 3.0f);
    if (still && m.calibrating) {
        m.drift = mean;
        m.calibrating = false;
        m.calibrated = true;
        m.calibration_deadline = 0;
        m.smoothed = {};
        LOGR("gyro: drift %.3f %.3f %.3f", m.drift.x, m.drift.y, m.drift.z);
    } else if (still && !m.calibrated && Largest(mean) < kMaxDrift) {
        m.drift = mean;
    }
    w = {};
}

// the slow share of a turn is smoothed, the fast share passes as it is: sensor noise goes, quick aim keeps no lag
Vec3 ApplySmoothing(Motion& m, const Vec3& turn, float dt) {
    float fast = std::clamp(Length(turn) / kSmoothSpeed - 1.0f, 0.0f, 1.0f);
    float k = dt / (kSmoothSeconds + dt);
    m.smoothed = Add(m.smoothed, Scale(Sub(Scale(turn, 1.0f - fast), m.smoothed), k));
    return Add(Scale(turn, fast), m.smoothed);
}

void ReadGyro(Motion& m, const Vec3& radians, uint64_t ns, bool phone = false) {
    float dt = Gap(m.gyro_ns, ns);
    Vec3 gyro = Scale(radians, kDegreesPerRadian);
    if (m.calibrating && SDL_GetTicksNS() >= m.calibration_deadline) {
        m.calibrating = false;
        m.calibration_failed = true;
    }
    UpdateDrift(m, gyro, dt, phone);
    if (m.calibrating) return;
    Vec3 turn = ApplySmoothing(m, Sub(gyro, m.drift), dt);
    m.yaw += turn.y * dt;
    m.roll += turn.z * dt;  // the left side down turns left
    m.world_yaw += Dot(turn, Up(m)) * dt;
    m.pitch -= turn.x * dt;  // turning about x raises the controller's front
}

void ReadSensor(Motion& m, const SDL_GamepadSensorEvent& s) {
    if (!m.pad && s.sensor == SDL_SENSOR_GYRO) m.pad = s.which;
    if (s.which != m.pad) return;
    Vec3 data{s.data[0], s.data[1], s.data[2]};
    uint64_t ns = s.sensor_timestamp ? s.sensor_timestamp : s.timestamp;
    if (s.sensor == SDL_SENSOR_GYRO) ReadGyro(m, data, ns);
    if (s.sensor == SDL_SENSOR_ACCEL) ReadAccel(m, data, ns);
}

void ReadButton(Motion& m, const SDL_GamepadButtonEvent& b) {
    if (b.which != m.pad || b.button >= 32) return;
    uint32_t bit = 1u << b.button;
    if (b.down && !(m.held & bit)) m.pressed |= bit;
    m.held = b.down ? m.held | bit : m.held & ~bit;
}

// SDL's thread that reads the controllers, every event as it arrives
bool SDLCALL OnEvent(void*, SDL_Event* e) {
    std::lock_guard lock(g_lock);
    switch (e->type) {
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (e->gdevice.which == g_motion.pad) g_motion = {};
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            ReadButton(g_motion, e->gbutton);
            break;
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            ReadSensor(g_motion, e->gsensor);
            break;
    }
    return true;
}

void EnableSensors() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; i++) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad || !SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) continue;
        if (SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) continue;
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
        LOGR("gyro: %s", SDL_GetGamepadName(pad));
    }
    SDL_free(ids);
}

// once the controllers are up: the event watch, then the sensors of new controllers each scan
void EnsureSensors() {
    static bool watching = false;
    static uint64_t scanned = 0;
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) return;
    if (!watching) watching = SDL_AddEventWatch(OnEvent, nullptr);
    uint64_t now = SDL_GetTicksNS();
    if (scanned && now - scanned < kScanNs) return;
    scanned = now;
    EnableSensors();
}

uint32_t ButtonBit(uint32_t index) {
    if (index == 0 || index >= std::size(kButtons)) return 0;
    return 1u << kButtons[index];
}

// the gyro button: off or on while held, or a press flips it; no button keeps the gyro on
bool UpdateSwitch(const Settings& s, const Motion& m) {
    uint32_t bit = ButtonBit(s.button);
    if (!bit) return true;
    bool held = m.held & bit;
    if (s.button_mode == kOnWhileHeld) return held;
    if (s.button_mode != kToggle) return !held;
    if (m.pressed & bit) g_toggled_off = !g_toggled_off;
    return !g_toggled_off;
}

float AxisTurn(const Motion& m, uint32_t axis) {
    if (axis == kRoll) return m.roll;
    if (axis == kWorld) return m.world_yaw;
    return m.yaw;
}

}  // namespace

Turn Take(const Settings& s) {
    static uint64_t last_take = 0;
#ifdef __ANDROID__
    bool phone = s.source == kPhone;
    if (s.enabled && !phone) EnsureSensors();
    std::lock_guard lock(g_lock);
    static uint32_t last_source = kController;
    static bool last_enabled = false;
    if (last_source != s.source || last_enabled != s.enabled) {
        g_motion = {};
        g_phone = {};
        g_toggled_off = false;
        last_take = 0;
        last_source = s.source;
        last_enabled = s.enabled;
    }
    if (!s.enabled) return {};
#else
    if (!s.enabled) return {};
    EnsureSensors();
    std::lock_guard lock(g_lock);
#endif
    uint64_t now = SDL_GetTicksNS();
    bool stale = now - last_take > kStaleNs;
    last_take = now;
#ifdef __ANDROID__
    Motion& m = phone ? g_phone : g_motion;
#else
    Motion& m = g_motion;
#endif
    Turn turn;
    if (stale) m.pressed = 0;  // presses while the look did not run
#ifdef __ANDROID__
    // Phone input has no controller-only activation/recenter buttons.
    bool on = phone ? g_phone_active : UpdateSwitch(s, m);
#else
    bool on = UpdateSwitch(s, m);
#endif
    if (on && !stale) {
        turn.yaw = AxisTurn(m, s.axis) * s.sensitivity * (s.invert_x ? -1.0f : 1.0f);
        turn.pitch = m.pitch * s.sensitivity * (s.invert_y ? -1.0f : 1.0f);
    }
    turn.recenter = !stale && (m.pressed & ButtonBit(s.recenter_button));
    m.yaw = 0;
    m.roll = 0;
    m.world_yaw = 0;
    m.pitch = 0;
    m.pressed = 0;
    return turn;
}

void StartCalibration(uint32_t source) {
    std::lock_guard lock(g_lock);
#ifdef __ANDROID__
    Motion& m = source == kPhone ? g_phone : g_motion;
#else
    Motion& m = g_motion;
#endif
    m.calibration_failed = !m.gyro_ns;
    if (!m.gyro_ns) return;  // no gyro read yet
    m.calibrating = true;
    m.calibration_deadline = SDL_GetTicksNS() + 10'000'000'000;
    m.rest = {};
    m.yaw = m.roll = m.world_yaw = m.pitch = 0;
}

bool Calibrating(uint32_t source) {
    std::lock_guard lock(g_lock);
#ifdef __ANDROID__
    return (source == kPhone ? g_phone : g_motion).calibrating;
#else
    return g_motion.calibrating;
#endif
}

const char* CalibrationStatus(uint32_t source) {
    std::lock_guard lock(g_lock);
#ifdef __ANDROID__
    Motion& m = source == kPhone ? g_phone : g_motion;
#else
    Motion& m = g_motion;
#endif
    if (m.calibrating && SDL_GetTicksNS() >= m.calibration_deadline) {
        m.calibrating = false;
        m.calibration_failed = true;
    }
    if (m.calibrating) return "KEEP STILL";
    if (m.calibration_failed) return "RETRY";
    return m.calibrated ? "DONE" : "START";
}

#ifdef __ANDROID__
void ResetPhone(bool active) {
    std::lock_guard lock(g_lock);
    g_phone = {};
    g_phone_active = active;
    g_phone_rotation = -1;
}

void PhoneSample(bool accel, float x, float y, float z, uint64_t ns, int rotation) {
    if (!ns || rotation < 0 || rotation > 3 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    std::lock_guard lock(g_lock);
    if (!g_phone_active) return;
    if (rotation != g_phone_rotation) {
        g_phone = {};  // old gravity, drift and pending turns use a different coordinate frame
        g_phone_rotation = rotation;
    }
    // Android natural-device axes -> screen right, screen top, out of screen.
    // Both vectors must use the same proper rotation for WORLD projection and calibration.
    Vec3 data;
    switch (rotation) {
        case 0: data = {x, y, z}; break;
        case 1: data = {-y, x, z}; break;
        case 2: data = {-x, -y, z}; break;
        default: data = {y, -x, z}; break;
    }
    uint64_t last = accel ? g_phone.accel_ns : g_phone.gyro_ns;
    if (ns <= last) return;  // duplicated/out-of-order samples must not rewind the integration clock
    if (accel) ReadAccel(g_phone, data, ns);
    else ReadGyro(g_phone, data, ns, true);
}
#endif

}  // namespace gyro
