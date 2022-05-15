# fakejoy

Fake a Linux input system joystick - almost.

## Why?

Beacuse I have a nice Logitech Freedom 2.4 wireless stick, with an annoying bug: if you don't touch the stick for 2 minutes it turns off to save battery,
which is fine except that the receiver unit does _not_ hold the last axis values, it resets to centred/zeroed.

This usually completely upsets the game you happen to be using it with (in my case Flightgear - full throttle randomly anyone?).

## How?

The code in `fakeev.c` passes through an existing device (my joystick), _except_ for that moment when the controls all reset to centre/zero -
at this point it drops those reports until things are moving again.

An earlier attempt using the Linux joystick API is in `fakejoy.c`, this only partly works and is a faff compared to the input event API, avoid.

A test program in `evdump.c` simply prints values from the requested device.

## Build / Run

Prerequisites: `build-essentials`, (`libcuse-dev` if you are interested in `fakejoy` - you are not!). Note that `fakeev` must run as
`root` to enable the creation of a new device node.. unless someone can come up with the magic kernel `CAPS` options to assign to a user?

```bash
% make
% ls -l bin
-rwxr-xr-x 1 phlash phlash 19784 Feb  3 15:11 evdump
-rwxr-xr-x 1 phlash phlash 22808 Feb  3 15:38 fakeev
% su
# bin/fakeev (it's a foreground program, not a daemon)
```
in another terminal
```bash
% bin/evdump /dev/input/event<N>
(where N is the latest device in /dev/input, check it reports the name '[Fakejoy] Logitech Freedom 2.4')
```

Go run your game and choose `[Fakejoy] Logitech Freedom 2.4` :smile:

## Details

`fakeev.c` uses the Linux `uinput` loopback API to emulate an input device, wrapping the real device and filtering reports. It passes through
all the device capabilities, axes, keys, etc. The filtering is specifically tailored to my Freedom 2.4, YYMV :smile:

NB: I have had to blacklist the `joydev` kernel driver (putting `blacklist joydev` in `/etc/modprobe.d/blacklist-joysticks.conf`) to prevent joystick API
devices appearing (as Flightgear cannot be configured to ignore them), again YMMV.

Specifically for Flightgear, you may also want to take the `test-joy-events.xml` and `testjoy.nas` files from the `flightgear` folder and place in your
home folder as `~/.fgfs/Input/Event/test-joy-events.xml` & `~/.fgfs/Nasal/testjoy.nas` respectively. This _should_ configure Flightgear to use the fake
joystick, and not the real one, and map the axes and controls to something saneish. Feel free to edit these files, they are quite self-explanatory.
