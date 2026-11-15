natpmp_add_test_executable(unit_tests
  c11.c
  main.c
  gateway.c
  mapping.c
  packet.c
  queue.c
  request.c
  retransmit.c
  session.c)

target_include_directories(unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/lib)

# The public header has to work for C11 users. COMPILE_OPTIONS come after the
# target's -std=, so they win. Pedantic, since GCC accepts some C23 in C11 as an
# extension.
set_source_files_properties(c11.c
  PROPERTIES COMPILE_OPTIONS "-std=c11;-pedantic-errors")
