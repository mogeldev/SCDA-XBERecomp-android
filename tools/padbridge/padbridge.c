// Emulator side of the controller bridge (padbridge.py): reads XINPUT_GAMEPAD states (12 bytes each) from stdin and
// plays them on a uinput device that Android takes for an Xbox 360 pad. Runs as root (adb root); EOF ends it.
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} Pad;

// XInput button bits to Linux keys, as the xpad driver reports a 360 pad
static const struct { uint16_t bit; int key; } kButtons[] = {
    {0x1000, BTN_A},      {0x2000, BTN_B},      {0x4000, BTN_X},      {0x8000, BTN_Y},
    {0x0100, BTN_TL},     {0x0200, BTN_TR},     {0x0020, BTN_SELECT}, {0x0010, BTN_START},
    {0x0040, BTN_THUMBL}, {0x0080, BTN_THUMBR}, {0x0400, BTN_MODE},
};

static int g_fd;

static void Emit(int type, int code, int value) {
    struct input_event e = {0};
    e.type = type;
    e.code = code;
    e.value = value;
    write(g_fd, &e, sizeof e);
}

static void Axis(int code, int min, int max) {
    struct uinput_abs_setup a = {0};
    a.code = code;
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    ioctl(g_fd, UI_ABS_SETUP, &a);
}

static int ReadAll(void* p, size_t n) {
    for (size_t got = 0; got < n;) {
        ssize_t r = read(0, (char*)p + got, n - got);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

int main(void) {
    g_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_fd < 0) {
        perror("padbridge: /dev/uinput");
        return 1;
    }
    ioctl(g_fd, UI_SET_EVBIT, EV_KEY);
    for (size_t i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++) ioctl(g_fd, UI_SET_KEYBIT, kButtons[i].key);
    ioctl(g_fd, UI_SET_EVBIT, EV_ABS);
    int axes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
    for (size_t i = 0; i < 8; i++) ioctl(g_fd, UI_SET_ABSBIT, axes[i]);
    Axis(ABS_X, -32768, 32767);
    Axis(ABS_Y, -32768, 32767);
    Axis(ABS_RX, -32768, 32767);
    Axis(ABS_RY, -32768, 32767);
    Axis(ABS_Z, 0, 255);
    Axis(ABS_RZ, 0, 255);
    Axis(ABS_HAT0X, -1, 1);
    Axis(ABS_HAT0Y, -1, 1);

    struct uinput_setup setup = {0};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x045E;
    setup.id.product = 0x028E;
    setup.id.version = 0x0114;
    strcpy(setup.name, "Microsoft X-Box 360 pad");
    if (ioctl(g_fd, UI_DEV_SETUP, &setup) < 0 || ioctl(g_fd, UI_DEV_CREATE) < 0) {
        perror("padbridge: uinput setup");
        return 1;
    }
    fprintf(stderr, "padbridge: pad created\n");

    Pad pad;
    while (ReadAll(&pad, sizeof pad)) {
        for (size_t i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++)
            Emit(EV_KEY, kButtons[i].key, !!(pad.buttons & kButtons[i].bit));
        // Linux sticks point down for positive y, XInput up
        Emit(EV_ABS, ABS_X, pad.lx);
        Emit(EV_ABS, ABS_Y, -1 - pad.ly);
        Emit(EV_ABS, ABS_RX, pad.rx);
        Emit(EV_ABS, ABS_RY, -1 - pad.ry);
        Emit(EV_ABS, ABS_Z, pad.lt);
        Emit(EV_ABS, ABS_RZ, pad.rt);
        Emit(EV_ABS, ABS_HAT0X, !!(pad.buttons & 0x8) - !!(pad.buttons & 0x4));
        Emit(EV_ABS, ABS_HAT0Y, !!(pad.buttons & 0x2) - !!(pad.buttons & 0x1));
        Emit(EV_SYN, SYN_REPORT, 0);
    }
    ioctl(g_fd, UI_DEV_DESTROY);
    return 0;
}
