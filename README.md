# obs-catpion - Real time speech to text plugin for OBS using libapril-asr

## About

This plugin captures audio from pipewire input sources and process it using libapril-asr to produce captions.

It's basically a mix of code taken from the following 3 projects.

* https://github.com/abb128/LiveCaptions
* https://github.com/dimtpap/obs-pipewire-audio-capture
* https://github.com/norihiro/obs-text-pthread

I also added this to implement sending text to other local apps through UDP/OSC protocol

* https://github.com/mhroth/tinyosc

## Install

On Fedora 39+ you can install through the COPR repository. 

```sh
dnf -y install 'dnf-command(copr)'
dnf -y copr enable grillo-delmal/obs-catpion
dnf -y install obs-studio-plugin-catpion
```

On Fedora 38 you need an extra COPR repository to be able to install the onnxruntime dependency

```sh
dnf -y install 'dnf-command(copr)'
dnf -y copr enable dherrera/onnx
dnf -y copr enable grillo-delmal/obs-catpion
dnf -y install obs-studio-plugin-catpion
```

I will consider packaging this properly in Fedora (and maybe Flatpak) when there is a
stable release of libapril-asr.

If you want to use it in other distro, feel free to build from source and try to mash it up yourself :)

## Build requirements

Currently this project depends on the following dependencies to build

* libobs
* libpipewire-0.3
* pango
* cairo
* pangocairo
* april-asr
* qt6-qtbase-devel

Most of them can be installed directly in Fedora

```sh
dnf -y install
    pipewire-devel \
    obs-studio-devel \
    "pkgconfig(pango)" \
    qt6-qtbase-devel
```

Installing [april-asr](https://github.com/abb128/april-asr) and [onnxruntime](https://github.com/abb128/april-asr#downloading-onnxruntime) might be a bit harder depending on your environment/system/distro.

In the case of Fedora I already have a pacakged version of april-asr in a COPR repo:

```sh
dnf -y install 'dnf-command(copr)'
dnf -y copr enable grillo-delmal/obs-catpion
dnf -y install april-asr-devel
```

## Build

Just use cmake? or check the [obs-catpion-build](https://github.com/grillo-delmal/obs-catpion-build) project.

```sh
mkdir build
cmake -S . -B build
cmake --build build
```

## Install

You can trust this command depending on your distro :)

```sh
sudo cmake --install build
```

Or you can try placing the files in the correct folders for your specific system.

In my case:

```sh
/usr/lib64/obs-plugins/obs-catpion.so
/usr/share/obs/obs-plugins/obs-catpion/locale/en-US.ini
/usr/share/obs/obs-plugins/obs-catpion/textalpha.effect
```

## FAQ

### So.... Do you mean obs-caption?

No, obs-catpion.

### Why Fedora and not XXX?

Because it's what I use and it's easier for me to deploy and test.

### Any similar proyects you can recommend?

Yes! check out obs-localvocal :D

* https://github.com/royshil/obs-localvocal

It works on multiple platforms and has many other different functionalities besides captioning.
The main difference is that whispercpp is built to process segmented chunks of audio at a time,
while april can process the audio stream as its being received. Both apporaches have advantages
and disadvantages depending on what you plan to do.
