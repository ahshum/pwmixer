cmake_minimum_required(VERSION 3.20)
project(PWMIXER)

set(CMAKE_C_STANDARD 23)

find_package(PkgConfig REQUIRED)
find_package(Curses REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

include_directories(
  ${PIPEWIRE_INCLUDE_DIRS}
  ${CURSES_INCLUDE_DIR})

add_subdirectory(src)
