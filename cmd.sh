#!/usr/bin/env sh

prepare() {
  [ ! -d "build" ] && mkdir build
  cd build
}

cmd="$1" && [ "$#" -ge 1 ] && shift

case "$cmd" in
  cmake)
    prepare
    cmake .. "$@"
    ;;

  mr)
    prepare
    cmake --build .
    [ "$?" -gt 0 ] && exit
    PIPEWIRE_LOG=pipewire.log \
    PIPEWIRE_DEBUG=T \
      ./src/pwmixer
    ;;

  t)
    prepare
    cmake .. "$@"
    cmake --build .
    cd test
    ctest
    ;;
esac
