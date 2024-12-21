set(MAIN pwmixer.c)

add_executable(pwmixer
  ${MAIN})

target_link_libraries(pwmixer
  m
  ${PIPEWIRE_LIBRARIES}
  ${CURSES_LIBRARIES})
