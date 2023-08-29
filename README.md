# obs-catpion - Real time speech to text plugin for OBS using libapril-asr

## About

This plugin captures audio from pipewire input sources and process it using libapril-asr to produce captions.

It's basically a mix of code taken from the following 3 projects.

https://github.com/abb128/LiveCaptions
https://github.com/dimtpap/obs-pipewire-audio-capture
https://github.com/norihiro/obs-text-pthread

I'm targeting this to work on obs for Fedora 38+.

## Build requirements

Currently this project depends on the following dependencies to build

* libobs
* libpipewire-0.3
* pango
* cairo
* pangocairo
* april-asr

Most of them can be installed directly in Fedora

```sh
dnf -y install
    pipewire-devel \
    obs-studio-devel \
    "pkgconfig(pango)"
```

Installing [april-asr](https://github.com/abb128/april-asr) and [onnxruntime](https://github.com/abb128/april-asr#downloading-onnxruntime) is a bit harder... good luck with that ;)

Also run the `prepare.sh` script to download the april model.
For now it's a hardcoded requirement that I plan to remove ASAP.

## Build

Just use cmake? or check the [obs-catpion-build](https://github.com/grillo-delmal/obs-catpion-build) project.

```sh
mkdir build
cmake -S . -B build
cmake --build build
```

## Install

You can try to trust

```sh
sudo cmake --install build
```

Or you can try placing the files in the correct folders for your specific system.

In my case:

```sh
/usr/lib64/obs-plugins/obs-catpion.so
/usr/share/obs/obs-plugins/obs-catpion
/usr/share/obs/obs-plugins/obs-catpion/april-english-dev-01110_en.april
/usr/share/obs/obs-plugins/obs-catpion/textalpha.effect
```

## TODO

* Load april model file on runtime
  * Add UI to select model file
  * Make things react on model change
* Add support for capturing audio from output and apps
* Package it for Fedora (or at least COPR)
* CI?

## FAQ

### So.... Do you mean obs-caption?

No, obs-catpion.

### Why Fedora38+ and not XXX?

Because it's what I use and it's easier for me to deploy and test.
