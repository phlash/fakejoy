#include <linux/input.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
	char *evdev = "/dev/input/by-id/usb-Logitech_Logitech_Freedom_2.4-event-joystick";
	if (argc>1)
		evdev = argv[1];
	printf("opening: %s\n", evdev);
	int efd = open(evdev, O_RDONLY);
	if (efd<0) {
		perror("opening device");
		return 1;
	}
	// read a bunch of info
	int ver;
	if (ioctl(efd, EVIOCGVERSION, &ver)) {
		perror("ioctl(EVIOCGVERSION)");
		return 1;
	}
	printf("driver version: %08x\n", ver);
	struct input_id id;
	if (ioctl(efd, EVIOCGID, &id)) {
		perror("ioctl(EVIOCGID)");
		return 1;
	}
	printf("device id: bus: %04x vendor: %04x product: %04x version: %04x\n",
		id.bustype, id.vendor, id.product, id.version);
	char name[128];
	int nlen = ioctl(efd, EVIOCGNAME(sizeof(name)), name);
	if (nlen<0) {
		perror("ioctl(EVIOCGNAME)");
		return 1;
	}
	printf("name: %.*s\n", nlen, name);
	char props[128];
	int plen = ioctl(efd, EVIOCGPROP(sizeof(props)), props);
	if (plen<0) {
		perror("ioctl(EVIOCGPROP)");
		return 1;
	}
	printf("props(%d): ", plen);
	for (int i=0; i<plen; i++) printf("%02x, ", props[i]);
	puts("");
	char kbit[KEY_CNT/8+1];
	char keys[KEY_CNT/8+1];
	int klen = ioctl(efd, EVIOCGBIT(EV_KEY,sizeof(kbit)), kbit);
	if (klen<0) {
		perror("ioctl(EVIOCGBIT)");
		return 1;
	}
	klen = ioctl(efd, EVIOCGKEY(sizeof(keys)), keys);
	if (klen<0) {
		perror("ioctl(EVIOCGKEY)");
		return 1;
	}
	printf("keys(%d): ", klen);
	for (int i=0; i<KEY_CNT; i++) {
		if (kbit[i/8] & (1<<(i%8))) {
			printf("%02x(%d),", i, keys[i/8] & (1<<(i%8)) ? 1 : 0);
		}
	}
	puts("");
	char abit[ABS_CNT/8+1];
	int alen = ioctl(efd, EVIOCGBIT(EV_ABS,sizeof(abit)), abit);
	if (alen<0) {
		perror("ioctl(EVIOCGBIT)");
		return 1;
	}
	printf("abs(%d):\n", alen);
	for (int i=0; i<ABS_CNT; i++) {
		if (abit[i/8] && (1<<(i%8))) {
			struct input_absinfo abs;
			if (ioctl(efd, EVIOCGABS(i), &abs)<0) {
				perror("ioctl(EVIOCGABS)");
				return 1;
			}
			printf("  %02x: %d<%d<%d %d/%d\n", i, abs.minimum, abs.value, abs.maximum, abs.flat, abs.fuzz);
		}
	}
	puts("");
	// print incoming events
	struct input_event ev;
	while (read(efd, &ev, sizeof(ev))>0) {
		printf("type: %04x code: %04x val: %i\n", ev.type, ev.code, ev.value);
	}
	return 0;
}
