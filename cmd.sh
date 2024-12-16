#!/usr/bin/env sh

cmd="$1" && [ "$#" -ge 1 ] && shift

case "$cmd" in
  cmake)
    [ ! -d "build/" ] && mkdir build/
    cd build
    cmake .. "$@"
    ;;

  mr)
    cd build
    make
    [ "$?" -gt 0 ] && exit
    PIPEWIRE_LOG=pipewire.log \
    PIPEWIRE_DEBUG=T \
      ./pwmixer
    ;;
esac
