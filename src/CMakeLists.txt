set(MAIN pwmixer.c)

set(SOURCES
  array.c)

set(HEADERS
  array.h)

add_library(PWMIXER
  ${HEADERS}
  ${SOURCES})

add_executable(pwmixer
  ${MAIN})

target_link_libraries(pwmixer
  m
  PWMIXER
  ${PIPEWIRE_LIBRARIES}
  ${CURSES_LIBRARIES})
