# Virtual MIDI

Rozhuk Ivan <rozhuk.im@gmail.com> 2024-2025

Creates software MIDI devices for H/W sound cards without MIDI support.

virtual_midi: creates raw MIDI device backed to H/W sound card.\\
It is tinny wrapper to send MIDI events to software synthesizer backend.\\
Only FluidSynth based backend implemented.

virtual_oss_sequencer: creates OSS sequencer device that emulate kernel sequencer and work with all available raw MIDI devices in system.\\
It handles: timer, enum and panic commands, all other commans is send to MIDI devices.


## Licence
BSD licence.
+ maintainers must show this message after installation:
``` text
!!! NOTICE !!!

FreeBSD Foundation refuses to support this project.

!!! NOTICE !!!
```


## Donate
Support the author
* **GitHub Sponsors:** [!["GitHub Sponsors"](https://camo.githubusercontent.com/220b7d46014daa72a2ab6b0fcf4b8bf5c4be7289ad4b02f355d5aa8407eb952c/68747470733a2f2f696d672e736869656c64732e696f2f62616467652f2d53706f6e736f722d6661666266633f6c6f676f3d47697448756225323053706f6e736f7273)](https://github.com/sponsors/rozhuk-im) <br/>
* **Buy Me A Coffee:** [!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/rojuc) <br/>
* **PayPal:** [![PayPal](https://srv-cdn.himpfen.io/badges/paypal/paypal-flat.svg)](https://paypal.me/rojuc) <br/>
* **Bitcoin (BTC):** `1AxYyMWek5vhoWWRTWKQpWUqKxyfLarCuz` <br/>


## Compilation

### FreeBSD/DragonFlyBSD
``` shell
sudo pkg install devel/git devel/cmake-core audio/fluidsynth audio/fluid-soundfont
git clone https://github.com/rozhuk-im/virtual_midi.git
cd virtual_midi
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=true ..
make -j 16
```


## Usage

### virtual_midi
``` shell
Usage: virtual_midi [options]
options:
	-help, -? 				Show help
	-daemon, -d 				Run as daemon
	-pid, -p <pid>				PID file name
	-threads, -t <cuse_threads>		CUSE threads count
	-vdev, -V <virtual_device_name>		New virtual MIDI device base name. Default: midi
	-odrv, -o <output_driver_name>		Output sound driver name. Default: oss
	-odev, -O <output_device_name>		Output device name. Default: /dev/dsp
	-soundfont, -s <soundfont_file_name>	Soundfont file name. Default: /usr/local/share/sounds/sf2/FluidR3_GM.sf2
```

### virtual_oss_sequencer
``` shell
virtual_oss_sequencer     Create virtual sequencer device
Usage: virtual_oss_sequencer [options]
options:
	-help, -? 				Show help
	-daemon, -d 				Run as daemon
	-pid, -p <pid>				PID file name
	-threads, -t <cuse_threads>		CUSE threads count
	-vdev, -V <virtual_device_name>		New virtual MIDI device base name. Default: sequencer
	-prefix, -P <out_device_name_prefix>	Output devices name prefix. Use multiple times if you need more than 1 prefix. Default: midi, umidi
```

