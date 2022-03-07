// Fake a Linux input event device, wrap a real one..
// in order to mask axis pertubations when my Logitech Freedom 2.4
// goes offline.
// Strategy: accumulate events until a sync event arrives, updating
// an internal state structure. When sync arrives, check state for
// offline indication (specific axes values), if so, set offline
// indicator; if not, reset offline indicator, copy new state to
// current state and push all dirty values through uinput loopback

#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

// raw joystick state
typedef struct {
    __s32 axes[ABS_CNT];
    __u8 keys[KEY_CNT];
    __u8 offline;
} joystate_t;

static volatile int done;
void trap(int sig) {
    fprintf(stderr, "SIG:%d\n", sig);
    done = 1;
}

typedef struct {
    __s32 min;
    __s32 max;
    __s32 dlow;
    __s32 dhigh;
} deadzone_t;

static __s32 deadzone(deadzone_t *zones, int axis, __s32 value) {
    deadzone_t *dz = zones+axis;
    __s32 rv = (dz->dlow+dz->dhigh)/2;
    if (value > dz->dhigh)
        rv += ((value - dz->dhigh) * (dz->max - rv)) / (dz->max - dz->dhigh);
    else if (value < dz->dlow)
        rv -= ((dz->dlow - value) * (rv - dz->min)) / (dz->dlow - dz->min);
    return rv;
}

static int send_event(int uifd, __u16 type, __u16 code, __s32 value) {
    struct input_event evt;
    gettimeofday(&evt.time, NULL);
    evt.type = type;
    evt.code = code;
    evt.value = value;
    if (write(uifd, &evt, sizeof(evt))!=sizeof(evt)) {
        if (EAGAIN==errno) {
            puts("uinput overflow");
        } else {
            perror("writing uinput");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    char *evdev = "/dev/input/by-id/usb-Logitech_Logitech_Freedom_2.4-event-joystick";
    char *fake = "[Fakejoy] Logitech Freedom 2.4";
    char *logf = "/tmp/fakeev.log";
    int backgnd = 0;
    for (int a=1; a<argc; a++) {
        if (strncmp(argv[a],"-d",2)==0)
            evdev = argv[++a];
        else if (strncmp(argv[a],"-f",2)==0)
            fake = argv[++a];
        else if (strncmp(argv[a],"-b",2)==0)
            backgnd = 1;
        else if (strncmp(argv[a],"-l",2)==0)
            logf = argv[++a];
        else
            return printf("usage: %s [-b [-l <logfile:%s>]] [-d <real device:%s>] [-f <fake device:%s>]\n", argv[0], logf, evdev, fake);
    }
    if (backgnd) {
        // fork/detach ourselves
        if (fork())
            return 0;
        // attach stdin to /dev/null, stdout/stderr output to a log file
        dup2(open("/dev/null", O_RDONLY), 0);
        dup2(open(logf, O_CREAT|O_APPEND|O_WRONLY, 0666), 1);
        dup2(1, 2);
        setsid();
    } else {
        // foreground - trap Ctrl-C
        signal(SIGINT, trap);
    }
    printf("opening real device: %s\n", evdev);
    int evfd = open(evdev, O_RDONLY);
    if (evfd<0) {
        perror("opening underlying device");
        return 1;
    }
    int uifd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uifd<0) {
        perror("opening /dev/uinput");
        return 1;
    }
    // set fake device name and bus id
    char name[128];
    int nlen=ioctl(evfd, EVIOCGNAME(sizeof(name)), name);
    if (nlen<0) {
        perror("reading device name");
        return 1;
    }
    printf("real device name: %.*s\n", nlen, name);
    struct uinput_setup setup;
    if (ioctl(evfd, EVIOCGID, &setup.id)<0) {
        perror("reading real bus id");
        return 1;
    }
    strncpy(setup.name, fake, UINPUT_MAX_NAME_SIZE);
    setup.id.bustype = BUS_VIRTUAL;     // Not a USB device, otherwise identical :=)
    setup.ff_effects_max = 0;           // No force feedback please!
    if (ioctl(uifd, UI_DEV_SETUP, &setup)<0) {
        perror("ioctl(UI_DEV_SETUP)");
        return 1;
    }
    printf("fake device name: %s\n", fake);
    // read capabilities, copy to fake device
    // we use KEY_CNT for array size here as it's the largest thing
    char bits[KEY_CNT];
    int blen;
    int blist[] = {0, EV_ABS, EV_KEY, EV_MAX, -1};
    int uiioc[] = {UI_SET_EVBIT, UI_SET_ABSBIT, UI_SET_KEYBIT, UI_SET_PROPBIT};
    int naxes;
    char axes[ABS_CNT];
    for (int b=0; blist[b]!=-1; b++) {
        if (EV_MAX==blist[b])
            blen = ioctl(evfd, EVIOCGPROP(sizeof(bits)), bits);
        else
            blen = ioctl(evfd, EVIOCGBIT(blist[b],sizeof(bits)), bits);
        if (blen<0) {
            perror("reading real device bits");
            return 1;
        }
        printf("copy bits(%d)=%d: ", blist[b], blen);
        for (int o=0; o<blen*8; o++) {
            if (bits[o/8] & (1<<(o%8))) {
                printf("%02x,", o);
                if (ioctl(uifd, uiioc[b], o)<0) {
                    perror("ioctl(UI_SET_XX)");
                    return 1;
                }
            }
        }
        puts("");
        // save axes bits for later
        if (EV_ABS==blist[b]) {
            naxes = blen;
            memcpy(axes, bits, naxes);
        }
    }
    // accumulated state & deadzones
    joystate_t joy = {0};
    deadzone_t zones[ABS_CNT];
    memset(zones, 0, sizeof(zones));
    // read axis info, pre-populate values, calculate deadzones
    for (int a=0; a<naxes*8; a++) {
        if (axes[a/8] & (1<<a%8)) {
            struct uinput_abs_setup abs_setup;
            if (ioctl(evfd, EVIOCGABS(a), &abs_setup.absinfo)<0) {
                perror("reading axis info");
                return 1;
            }
            abs_setup.code = a;
            if (ioctl(uifd, UI_ABS_SETUP, &abs_setup)<0) {
                perror("ioctl(UI_ABS_SETUP)");
                return 1;
            }
            joy.axes[a] = abs_setup.absinfo.value;
            zones[a].min = abs_setup.absinfo.minimum;
            zones[a].max = abs_setup.absinfo.maximum;
            zones[a].dlow = (abs_setup.absinfo.minimum+abs_setup.absinfo.maximum)/2-abs_setup.absinfo.flat;
            zones[a].dhigh = (abs_setup.absinfo.minimum+abs_setup.absinfo.maximum)/2+abs_setup.absinfo.flat;
            printf("axis[%d]: min=%d max=%d fuzz=%d flat=%d: dlow=%d dhigh=%d\n", a,
                abs_setup.absinfo.minimum,
                abs_setup.absinfo.maximum,
                abs_setup.absinfo.fuzz,
                abs_setup.absinfo.flat,
				zones[a].dlow, zones[a].dhigh);
        }
    }
    // create the fake device!
    if (ioctl(uifd, UI_DEV_CREATE)<0) {
        perror("creating fake device");
        return 1;
    }
    // read events, update accumulated state..
    joystate_t pjoy = {0};
    int first = 1;
    while (!done) {
        struct input_event evt;
        if (first) {
            // fake a SYN to push initial state out
            first = 0;
            evt.type = EV_SYN;
            evt.code = 0;
            evt.value = 0;
        } else if (read(evfd, &evt, sizeof(evt))!=sizeof(evt)) {
            if (!done)
                perror("reading event");
            break;
        }
        switch (evt.type) {
        // drop through to sync logic
        case EV_SYN:
            break;
        // update accumulated state, go round again
        case EV_ABS:
            joy.axes[evt.code % ABS_CNT] = evt.value;
            continue;
        case EV_KEY:
            joy.keys[evt.code % KEY_CNT] = (__u8)evt.value;
            continue;
        // siliently ignore these, we get one after each key press/release
        case EV_MSC:
            continue;
        // eh?
        default:
            printf("ignored event: type=0x%x code=0x%x value=%d\n", evt.type, evt.code, evt.value);
            continue;
        }
        // EV_SYN arrived, check for magic offline values
        // X & Y within +/-2 of centre (512), rudder centre (128) throttle full (0)
        if (510<=joy.axes[ABS_X] && joy.axes[ABS_X]<=514 &&
            510<=joy.axes[ABS_Y] && joy.axes[ABS_Y]<=514 &&
            128==joy.axes[ABS_RZ] && 0==joy.axes[ABS_THROTTLE]) {
            joy.offline = 1;
        } else {
            joy.offline = 0;
        }
        // push modified values to uinput unless offline
        if (!joy.offline) {
            for (int a=0; a<ABS_CNT; a++) {
                if (pjoy.axes[a] != joy.axes[a]) {
                    pjoy.axes[a] = joy.axes[a];
                    if (send_event(uifd, EV_ABS, a, deadzone(zones, a, pjoy.axes[a])))
                        done = 1;
                }
            }
            for (int k=0; k<KEY_CNT; k++) {
                if (pjoy.keys[k] != joy.keys[k]) {
                    pjoy.keys[k] = joy.keys[k];
                    if (send_event(uifd, EV_KEY, k, pjoy.keys[k]))
                        done = 1;
                }
            }
            // push SYN to flush out
            send_event(uifd, EV_SYN, SYN_REPORT, 0);
        }
        if (!backgnd) printf("X:%04d Y:%04d R:%03d T:%03d B:%d%d%d%d%d%d%d%d%d%d H:%c%c O:%d\r",
            deadzone(zones, ABS_X, pjoy.axes[ABS_X]),
            deadzone(zones, ABS_Y, pjoy.axes[ABS_Y]),
            deadzone(zones, ABS_RZ, pjoy.axes[ABS_RZ]),
            deadzone(zones, ABS_THROTTLE, pjoy.axes[ABS_THROTTLE]),
            pjoy.keys[BTN_TRIGGER], pjoy.keys[BTN_THUMB], pjoy.keys[BTN_THUMB2],
            pjoy.keys[BTN_TOP], pjoy.keys[BTN_TOP2], pjoy.keys[BTN_PINKIE],
            pjoy.keys[BTN_BASE], pjoy.keys[BTN_BASE2], pjoy.keys[BTN_BASE3], pjoy.keys[BTN_BASE4],
            '='+pjoy.axes[ABS_HAT0X], '='+pjoy.axes[ABS_HAT0Y], joy.offline);
            fflush(stdout);
    }
    ioctl(uifd, UI_DEV_DESTROY);
    close(uifd);
    close(evfd);
    fprintf(stderr, "fakeev: terminating\n");
    return 0;
}
