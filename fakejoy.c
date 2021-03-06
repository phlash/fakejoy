#define FUSE_USE_VERSION 31
#include <cuse_lowlevel.h>
#include <linux/joystick.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>

static int s_realfd;
static int s_reads;
static uint32_t s_version = JS_VERSION;
static char s_name[] = "[Fakejoy] Logitech Freedom 2.4";

// tracked state (used to detect disconnect)
static struct {
	uint8_t btns[KEY_CNT-BTN_MISC];
	int16_t axes[ABS_CNT];
	uint8_t nbtns;
	uint8_t naxes;
	uint32_t last;
	int lcnt;
	int conn;
} s_fakejoy;

static int fakejoy_detect(struct js_event *js) {
	// update tracked state
	switch (js->type & ~JS_EVENT_INIT) {
	case JS_EVENT_BUTTON:
		s_fakejoy.btns[js->number] = (uint8_t)js->value;
		break;
	case JS_EVENT_AXIS:
		s_fakejoy.axes[js->number] = js->value;
		break;
	}
	if (js->time == s_fakejoy.last)
		s_fakejoy.lcnt += 1;
	else {
		s_fakejoy.last = js->time;
		s_fakejoy.lcnt = 0;
	}
	// we determine connection state as follows:
	// 3+ events with the same timestamp
	// if axis 3 is -32767, then disconnected, otherwise connected
	if (s_fakejoy.lcnt > 2) {
		if (-32767==s_fakejoy.axes[3])
			s_fakejoy.conn = 0;
		else
			s_fakejoy.conn = 1;
	}
	/*printf("fakejoy_detect: type=0x%x btns=", js->type);
	for (int b=0; b<s_fakejoy.nbtns; b++)
		printf("%d,", s_fakejoy.btns[b]);
	printf(" axes=");
	for (int a=0; a<s_fakejoy.naxes; a++)
		printf("%d,", s_fakejoy.axes[a]);
	printf(" last=%u lcnt=%d conn=%d\n", s_fakejoy.last, s_fakejoy.lcnt, s_fakejoy.conn);*/
	return s_fakejoy.conn;
}

static void fakejoy_open(fuse_req_t req, struct fuse_file_info *fi) {
	if (s_realfd>0) {
		puts("fakejoy_open: busy");
		fuse_reply_err(req, EBUSY);
		return;
	}
	char *realjoy = getenv("REALDEV");
	realjoy = realjoy ? realjoy : "/dev/input/js0";
	s_realfd = open(realjoy, O_RDONLY);
	if (s_realfd<0) {
		perror("fakejoy_open");
		fuse_reply_err(req, ENODEV);
		return;
	}
	if (ioctl(s_realfd, JSIOCGBUTTONS, &s_fakejoy.nbtns) ||
		ioctl(s_realfd, JSIOCGAXES, &s_fakejoy.naxes)) {
		puts("fakejoy_open: unable to read button/axes counts");
		fuse_reply_err(req, ENODEV);
		return;
	}
	// assume connected until proven otherwise
	s_fakejoy.lcnt = 0;
	s_fakejoy.conn = 1;
	printf("fakejoy_open(flags=0x%x): ok\n", fi->flags);
	fuse_reply_open(req, fi);
}

static void fakejoy_close(fuse_req_t req, struct fuse_file_info *fi) {
	(void)fi;
	if (s_realfd>0)
		close(s_realfd);
	s_realfd = 0;
	puts("\nfakejoy_close: ok");
	fuse_reply_err(req, 0);
}

static int s_lconn;
static void fakejoy_read(fuse_req_t req, size_t size, off_t off,
			 struct fuse_file_info *fi) {
	(void)off;
	struct js_event js;
	if (size < sizeof(js)) {
		printf("fakejoy_read: buffer too small (%u)\n", size);
		fuse_reply_err(req, EIO);
		return;
	}
	int canret = 0;
	while (!canret) {
		// poll for data, then decide action based on..
		int avail = 0;
		struct pollfd pfd = { s_realfd, POLLIN, 0 };
		if (poll(&pfd, 1, 0)>0 && (pfd.revents&POLLIN))
			avail = 1;
		// no data available & non-blocking mode, bail
		if (!avail && (fi->flags & O_NONBLOCK)) {
			fuse_reply_err(req, EAGAIN);
			return;
		}
		// otherwise go read (possibly blocking)
		int err = read(s_realfd, &js, sizeof(js));
		if (err!=sizeof(js)) {
			perror("fakejoy_read");
			fuse_reply_err(req, err);
			return;
		}
		// detect disconnect, drop events while disconnected
		if (!fakejoy_detect(&js)) {
			if (s_lconn)
				puts("fakejoy_read:disconnect");
			s_lconn = 0;
			// non-blocking mode, we're done
			if (fi->flags & O_NONBLOCK) {
				fuse_reply_err(req, EAGAIN);
				return;
			}
		} else {
			if (!s_lconn)
				puts("fakejoy_read:connect");
			s_lconn = 1;
			canret = 1;
		}
	}
	//printf("fakejoy_read(flags=0x%x size=%d): %d  \r", fi->flags, size, s_reads++);
	fflush(stdout);
	fuse_reply_buf(req, (const char *)&js, sizeof(js));
}

static void fakejoy_getx(int cmd, fuse_req_t req) {
	uint8_t val;
	int err = ioctl(s_realfd, cmd, &val);
	if (err<0) {
		perror("fakejoy_getx");
		fuse_reply_err(req, err);
		return;
	}
	char *act = "huh?";
	switch (cmd) {
	case JSIOCGAXES:
		act = "axes";
		break;
	case JSIOCGBUTTONS:
		act = "buttons";
		break;
	}
	printf("getx(%s)=%d\n", act, val);
	fuse_reply_ioctl(req, 0, &val, sizeof(val));
}

// for some batsh*t crazy reason we have to tell CUSE the size of operands
// we expect for ioctl's as it doesn't seem to know..
static void fakejoy_retry(fuse_req_t req, void *arg, int inout, size_t size) {
	struct iovec iov = { arg, size };
	printf("retry: inout=%d size=%d\n", inout, size);
	if (inout)	// output needed
		fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
	else		// input needed
		fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
}

static uint8_t s_buf[0x400];

static void fakejoy_ioctl(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
	printf("fakejoy_ioctl(cmd=%x size=%d in_bufsz=%d out_bufsz=%d) => ",
		cmd, _IOC_SIZE(cmd), in_bufsz, out_bufsz);
	int err = 0;
	switch (cmd) {
	case JSIOCGAXES:
	case JSIOCGBUTTONS:
		if (!out_bufsz)
			fakejoy_retry(req, arg, 1, sizeof(char));
		else
			fakejoy_getx(cmd, req);
		return;
	case JSIOCGVERSION:
		if (!out_bufsz)
			fakejoy_retry(req, arg, 1, sizeof(s_version));
		else {
			printf("getversion=%x\n", s_version);
			fuse_reply_ioctl(req, 0, &s_version, sizeof(s_version));
		}
		return;
	}
	switch (cmd & ~IOCSIZE_MASK) {
	case (JSIOCSAXMAP & ~IOCSIZE_MASK):
		if (!in_bufsz)
			fakejoy_retry(req, arg, 0, _IOC_SIZE(cmd));
		else {
			err = ioctl(s_realfd, cmd, in_buf);
			printf("setaxmap (err=%d)\n", err);
			fuse_reply_ioctl(req, err, NULL, 0);
		}
		return;
	case (JSIOCGAXMAP & ~IOCSIZE_MASK):
		if (!out_bufsz)
			fakejoy_retry(req, arg, 1, _IOC_SIZE(cmd));
		else {
			err = ioctl(s_realfd, cmd, s_buf);
			printf("getaxmap (err=%d)\n", err);
			fuse_reply_ioctl(req, 0, s_buf, _IOC_SIZE(cmd));
		}
		return;
	case (JSIOCSBTNMAP & ~IOCSIZE_MASK):
		if (!in_bufsz)
			fakejoy_retry(req, arg, 0, _IOC_SIZE(cmd));
		else {
			err = ioctl(s_realfd, cmd, in_buf);
			printf("setbtnmap (err=%d)\n", err);
			fuse_reply_ioctl(req, err, NULL, 0);
		}
		return;
	case (JSIOCGBTNMAP & ~IOCSIZE_MASK):
		if (!out_bufsz)
			fakejoy_retry(req, arg, 1, _IOC_SIZE(cmd));
		else {
			err = ioctl(s_realfd, cmd, s_buf);
			printf("getbtnmap (err=%d)\n", err);
			fuse_reply_ioctl(req, 0, s_buf, _IOC_SIZE(cmd));
		}
		return;
	case JSIOCGNAME(0):
		if (!out_bufsz) {
			fakejoy_retry(req, arg, 1, sizeof(s_name));
			return;
		}
		if (_IOC_SIZE(cmd) < sizeof(s_name)) {
			printf("getname: buffer too small (%u)\n", _IOC_SIZE(cmd));
			fuse_reply_err(req, EIO);
			return;
		}
		printf("getname='%.*s'\n", sizeof(s_name), s_name);
		fuse_reply_ioctl(req, 0, s_name, sizeof(s_name));
		return;
	default:
		printf("fakejoy_ioctl:unknown (%x)\n", cmd);
		fuse_reply_err(req, EINVAL);
	}
}

static const struct cuse_lowlevel_ops cuse_ops = {
	.open = fakejoy_open,
	.read = fakejoy_read,
	.ioctl= fakejoy_ioctl,
	.release = fakejoy_close,
};

void hide() {
	char *realjoy = getenv("REALDEV");
	realjoy = realjoy ? realjoy : "/dev/input/js0";
	chmod(realjoy, 0600);
}

void unhide() {
	char *realjoy = getenv("REALDEV");
	realjoy = realjoy ? realjoy : "/dev/input/js0";
	puts("unhide");
	chmod(realjoy, 0664);
}

int main(int argc, char **argv) {
	char dev_name[128] = "DEVNAME=";
	const char *dev_argv[] = { dev_name };
	struct cuse_info ci;
	// override default device name
	if (getenv("DEVNAME")!=NULL) {
		strncat(dev_name, getenv("DEVNAME"), 120);
	} else {
		strcat(dev_name, "js1");
	}
	// fill out the info struct
	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;
	hide();
	cuse_lowlevel_main(argc, argv, &ci, &cuse_ops, NULL);
	unhide();
	return 0;
}
