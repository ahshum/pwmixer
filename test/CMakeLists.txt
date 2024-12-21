include_directories(
  ${PWMIXER_SOURCE_DIR}/src)

add_executable(pwmixer_test
  pwmixer_test.c)

target_link_libraries(pwmixer_test
  PWMIXER)

enable_testing()
add_test(NAME pwmixer COMMAND pwmixer_test)
