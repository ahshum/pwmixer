cmake_minimum_required(VERSION 3.20)
project(pwmixer C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
find_package(Curses REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

include_directories(
  ${PIPEWIRE_INCLUDE_DIRS}
  ${CURSES_INCLUDE_DIR})

set(SOURCES
  src/pwmixer.c)

add_executable(pwmixer
  ${SOURCES})

target_link_libraries(pwmixer
  m
  ${PIPEWIRE_LIBRARIES}
  ${CURSES_LIBRARIES})
