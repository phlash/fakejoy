// Fake a Linux input event device, wrap a real one..
// in order to mask axis pertubations when my Logitech Freedom 2.4
// goes offline.
// Strategy: accumulate events until a sync event arrives, updating
// an internal state structure. When sync arrives, check state for
// offline indication (specific axes values), if so, set offline
// indicator; if not, reset offline indicator, copy new state to
// current state and mark all dirty values.
// Read-side, deliver dirty values as new events, or block/EAGAIN.

#define FUSE_USE_VERSION 31
#include <cuse_lowlevel.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <errno.h>

// raw joystick state
typedef struct {
    __s32 axes[ABS_CNT];
    __u8 keys[KEY_CNT];
    __u8 offline;
} joystate_t;

// joystick properties (as read)
typedef struct {
    // capabilities
    __u8 evts[EV_CNT/8];
    int elen;
    __u8 axes[ABS_CNT/8];
    int alen;
    __u8 keys[KEY_CNT/8];
    int klen;
    // properties
    __u8 prps[INPUT_PROP_CNT/8];
    int plen;
    // axis info
    struct input_absinfo ainf[ABS_CNT];
} joyprops_t;

// dirty bit queue - each 'bit' is a combination of axis(0)/key(1) in bit [31] and array index [30:0]
#define MAX_DIRTY   256
#define DIRTY_KEY   0x8000000
typedef struct {
    int head;
    int tail;
    __u32 bits[MAX_DIRTY];
} dirtyq_t;

// our fake device name & version
static char s_name[] = "[Fakejoy] Logitech Freedom 2.4";
static int s_version = EV_VERSION;

// access lock to shared state below
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
// properties from underlying device
static joyprops_t s_props;
// our current state and dirty bits
static joystate_t s_joy;
static dirtyq_t s_dirty;
// pollhandle from CUSE
static struct fuse_pollhandle *s_poll;
// reciever state
static int s_open;
static int s_sync;

// call only while holding s_lock
static void push_dirty(__u32 v) {
    if (!s_open)
        return;
    int nh = (s_dirty.head+1)%MAX_DIRTY;
    if (nh==s_dirty.tail) {
        puts("event queue full - dropping");
        return;
    }
    s_dirty.bits[s_dirty.head] = v;
    s_dirty.head = nh;
    // wake up any polls..
    if (s_poll) {
        fuse_lowlevel_notify_poll(s_poll);
        fuse_pollhandle_destroy(s_poll);
        s_poll = NULL;
    }
}

static __u32 pull_dirty() {
    if (s_dirty.tail==s_dirty.head)
        return (__u32)-1;
    __u32 rv = s_dirty.bits[s_dirty.tail];
    s_dirty.tail = (s_dirty.tail+1)%MAX_DIRTY;
    return rv;
}

static void *strategy_thread(void *arg) {
    int fdcmd = *(int *)arg;
    char *evdev = "/dev/input/by-id/usb-Logitech_Logitech_Freedom_2.4-event-joystick";
    if (getenv("FAKEJOY_EVDEV")!=NULL)
        evdev = getenv("FAKEJOY_EVDEV");
    printf("opening: %s\n", evdev);
    int evfd = open(evdev, O_RDONLY);
    if (evfd<0) {
        perror("opening underlying device");
        return NULL;
    }
    char name[128];
    int nlen=ioctl(evfd, EVIOCGNAME(sizeof(name)), name);
    if (nlen<0) {
        perror("reading device name");
        return NULL;
    }
    // read capabilities
    if ((s_props.elen=ioctl(evfd, EVIOCGBIT(0,sizeof(s_props.evts)),s_props.evts))<0 ||
        (s_props.klen=ioctl(evfd, EVIOCGBIT(EV_KEY,sizeof(s_props.keys)),s_props.keys))<0 ||
        (s_props.alen=ioctl(evfd, EVIOCGBIT(EV_ABS,sizeof(s_props.axes)),s_props.axes))<0 ||
        (s_props.plen=ioctl(evfd, EVIOCGPROP(sizeof(s_props.prps)),s_props.prps))<0) {
        perror("reading caps");
        return NULL;
    }
    // accumulated state
    joystate_t joy = {0};
    // read axis info, pre-populate values
    for (int a=0; a<s_props.alen*8; a++) {
        memset(&s_props.ainf[a], 0, sizeof(struct input_absinfo));
        if (s_props.axes[a/8] & (1<<a%8)) {
            if (ioctl(evfd, EVIOCGABS(a), &s_props.ainf[a])<0) {
                perror("reading axis info");
                return NULL;
            }
            pthread_mutex_lock(&s_lock);
            s_joy.axes[a] = joy.axes[a] = s_props.ainf[a].value;
            pthread_mutex_unlock(&s_lock);
        }
    }
    printf("device\n  name: %.*s\n  events: %d\n  keys: %d\n  axes: %d\n  props: %d\n",
        nlen, name, s_props.elen, s_props.klen, s_props.alen, s_props.plen);
    // read events, update accumulated state..
    struct pollfd pfds[2];
    for (;;) {
        pfds[0].fd = fdcmd;
        pfds[0].events = POLLIN;
        pfds[1].fd = evfd;
        pfds[1].events = POLLIN;
        if (poll(pfds, 2, -1)<0) {
            perror("polling for input");
            break;
        }
        // anything on fdcmd means we're done
        if (pfds[0].revents & POLLIN) {
            puts("\ntermination requested");
            break;
        }
        // process an event
        struct input_event evt;
        if (read(evfd, &evt, sizeof(evt))!=sizeof(evt)) {
            perror("reading event");
            break;
        }
        switch (evt.type) {
        // drop through to sync logic
        case EV_SYN:
            break;
        // update accumulated state, go round again
        case EV_ABS:
            joy.axes[evt.code % ABS_MAX] = evt.value;
            continue;
        case EV_KEY:
            joy.keys[evt.code % KEY_MAX] = (__u8)evt.value;
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
        // update shared state
        pthread_mutex_lock(&s_lock);
        s_joy.offline = joy.offline;
        if (!joy.offline) {
            for (int a=0; a<ABS_CNT; a++) {
                if (s_joy.axes[a] != joy.axes[a]) {
                    s_joy.axes[a] = joy.axes[a];
                    push_dirty(a);
                }
            }
            for (int k=0; k<KEY_CNT; k++) {
                if (s_joy.keys[k] != joy.keys[k]) {
                    s_joy.keys[k] = joy.keys[k];
                    push_dirty(DIRTY_KEY|k);
                }
            }
        }
        pthread_mutex_unlock(&s_lock);
        printf("X:%04d Y:%04d R:%03d T:%03d B:%d%d%d%d%d%d%d%d%d%d H:%c%c O:%d\r",
            joy.axes[ABS_X], joy.axes[ABS_Y], joy.axes[ABS_RZ], joy.axes[ABS_THROTTLE],
            joy.keys[BTN_TRIGGER], joy.keys[BTN_THUMB], joy.keys[BTN_THUMB2],
            joy.keys[BTN_TOP], joy.keys[BTN_TOP2], joy.keys[BTN_PINKIE],
            joy.keys[BTN_BASE], joy.keys[BTN_BASE2], joy.keys[BTN_BASE3], joy.keys[BTN_BASE4],
            '='+joy.axes[ABS_HAT0X], '='+joy.axes[ABS_HAT0Y], joy.offline);
        fflush(stdout);
    }
    close(evfd);
    return NULL;
}

// CUSE handlers
static void fakeev_open(fuse_req_t req, struct fuse_file_info *fi) {
    pthread_mutex_lock(&s_lock);
    int was_open = s_open;
    if (!s_open) {
        s_open += 1;
        s_dirty.head = s_dirty.tail = 0;    // prevent deluge of events after opening
    }
    pthread_mutex_unlock(&s_lock);
    if (was_open) {
        puts("Sorry - exclusive device use only");
        fuse_reply_err(req, EBUSY);
    } else {
        s_sync = 0;
        fuse_reply_open(req, fi);
    }
}

static void fakeev_close(fuse_req_t req, struct fuse_file_info *fi) {
    pthread_mutex_lock(&s_lock);
    s_open = 0;
    pthread_mutex_unlock(&s_lock);
}

static void fakeev_read(fuse_req_t req, size_t size, off_t off,
			 struct fuse_file_info *fi) {
    // dumb check
    if (size < sizeof(struct input_event)) {
        fuse_reply_err(req, EINVAL);
        return;
    }
    int wait = 1;
    while(wait) {
        pthread_mutex_lock(&s_lock);
        // pull events according to the dirty bits..
        __u32 db = pull_dirty();
        if ((__u32)-1 == db) {
            // synthesise sync when out of events
            if (!s_sync) {
                struct input_event evt;
                evt.type = EV_SYN;
                evt.code = 0;
                evt.value = 0;
                fuse_reply_buf(req, (const char *)&evt, sizeof(evt));
                s_sync = 1;
                wait = 0;
            }
            // block or EAGAIN?
            else if (fi->flags & O_NONBLOCK) {
                fuse_reply_err(req, EAGAIN);
                wait = 0;
            }
        } else {
            struct input_event evt;
            if (db & DIRTY_KEY) {
                db &= ~DIRTY_KEY;
                evt.type = EV_KEY;
                evt.code = db;
                evt.value = (__s32)s_joy.keys[db];
            } else {
                evt.type = EV_ABS;
                evt.code = db;
                evt.value = s_joy.axes[db];
            }
            fuse_reply_buf(req, (const char *)&evt, sizeof(evt));
            s_sync = 0;
            wait = 0;
        }
        pthread_mutex_unlock(&s_lock);
        if (wait) {
            struct timespec tv = { 0, 1000000 }; // 1ms
            nanosleep(&tv, NULL);
        }
    }
}

static void fakeev_retry(fuse_req_t req, void *arg, int inout, size_t size) {
	struct iovec iov = { arg, size };
	if (inout)	// output needed
		fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
	else		// input needed
		fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
}

static void ioctl_print(const char *pfx, int cmd) {
    printf("%s dir=%d type='%c' nr=0x%x size=0x%x\n", pfx, _IOC_DIR(cmd), _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd));
}

static void fakeev_ioctl(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
    switch (cmd) {
    case EVIOCGVERSION:
        if (!out_bufsz)
            fakeev_retry(req, arg, 1, sizeof(s_version));
        else
            fuse_reply_ioctl(req, 0, &s_version, sizeof(s_version));
        return;
    case EVIOCGID:
        if (!out_bufsz)
            fakeev_retry(req, arg, 1, sizeof(struct input_id));
        else {
            struct input_id id = {0};
            fuse_reply_ioctl(req, 0, &id, sizeof(id));
        }
        return;
    case EVIOCGRAB:
        fuse_reply_err(req, 0);
        return;
    }
    size_t rsize = _IOC_SIZE(cmd);
    #define MSIZE(sz) (rsize < (sz) ? rsize : (sz))
    switch (cmd & ~IOCSIZE_MASK) {
    case EVIOCGNAME(0): {
        size_t msize = MSIZE(sizeof(s_name));
        if (!out_bufsz)
            fakeev_retry(req, arg, 1, msize);
        else
            fuse_reply_ioctl(req, msize, s_name, msize);
        }
        return;
    // properties
    case EVIOCGPROP(0): {
        size_t msize = MSIZE(s_props.plen);
        if (!out_bufsz)
            fakeev_retry(req, arg, 1, msize);
        else
            fuse_reply_ioctl(req, msize, s_props.prps, msize);
        }  
        return;
    // capabilities..
    case EVIOCGBIT(0,0):
    case EVIOCGBIT(EV_ABS,0):
    case EVIOCGBIT(EV_KEY,0): {
        int nr = _IOC_NR(cmd) & EV_MAX;
        size_t ns = 0==nr ? s_props.elen : EV_ABS==nr ? s_props.alen : s_props.klen;
        ns = MSIZE(ns);
        if (!out_bufsz)
            fakeev_retry(req, arg, 1, ns);
        else
            fuse_reply_ioctl(req, ns, 0==nr ? s_props.evts : EV_ABS==nr ? s_props.axes : s_props.keys, ns);
        }
        return;
    case EVIOCGKEY(0): {
        size_t msize = MSIZE(KEY_CNT/8);
        pthread_mutex_lock(&s_lock);
        if (!out_bufsz)
            fakeev_retry(req, arg, 1, msize);
        else {
            char kbits[KEY_CNT/8] = {0};
            for (int k=0; k<msize*8; k++) {
                if (s_joy.keys[k])
                    kbits[k/8] |= (1<<(k%8));
            }
            fuse_reply_ioctl(req, msize, kbits, msize);
        }
        pthread_mutex_unlock(&s_lock);
        }
        return;
    }
    int tc = cmd & ~(ABS_MAX<<_IOC_NRSHIFT);
    int ioc = EVIOCGABS(0);
    if (tc == ioc) {
        if (!out_bufsz)
            fakeev_retry(req, arg, 1, sizeof(struct input_absinfo));
        else {
            struct input_absinfo info;
            int ax = _IOC_NR(cmd) & ABS_MAX;
            memcpy(&info, &s_props.ainf[ax], sizeof(info));
            pthread_mutex_lock(&s_lock);
            info.value = s_joy.axes[ax];
            pthread_mutex_unlock(&s_lock);
            fuse_reply_ioctl(req, 0, &info, sizeof(info));
        }
        return;
    }
    ioctl_print("unknown:", cmd);
    fuse_reply_err(req, EINVAL);
}

static void fakeev_poll(fuse_req_t req, struct fuse_file_info *fi,
		      struct fuse_pollhandle *ph) {
    pthread_mutex_lock(&s_lock);
    // discard old poll handle first..
    if (s_poll)
        fuse_pollhandle_destroy(s_poll);
    s_poll = ph;
    pthread_mutex_unlock(&s_lock);
    fuse_reply_poll(req, POLLIN);
}

static const struct cuse_lowlevel_ops cuse_ops = {
	.open = fakeev_open,
	.read = fakeev_read,
	.ioctl= fakeev_ioctl,
    .poll = fakeev_poll,
	.release = fakeev_close,
};

int main(int argc, char **argv) {
	char dev_name[128] = "DEVNAME=";
	const char *dev_argv[] = { dev_name };
	// override default device name
	if (getenv("DEVNAME")!=NULL) {
		strncat(dev_name, getenv("DEVNAME"), 120);
	} else {
		strcat(dev_name, "cuse99");    // TODO: HHHMNN?
	}
    // create strategy thread
    int pfds[2];
    pthread_t pid;
    pipe(pfds);
    pthread_create(&pid, NULL, strategy_thread, &pfds[0]);
	// fill out the info struct
	struct cuse_info ci;
	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;
	cuse_lowlevel_main(argc, argv, &ci, &cuse_ops, NULL);
    // collect the strategy thread
    write(pfds[1], pfds, 1);
    pthread_join(pid, NULL);
    return 0;
}
