# PWMixer

A lightweight Linux ncurses-based PipeWire mixer.

Inspired by pulsemixer.

## Dependencies

- pipewire
- ncurses

## Build

### ArchLinux

```
pacman -Ss base-devel cmake ncurses libpipewire
```

### Debian

```
apt-get install build-essential cmake pkg-config libpipewire-0.3-dev libncurses6 libncurses-dev
```

### Compile

```
mkdir build/
cd build/
cmake ..
make

# run
./pwmixer
```
