# The client end to end in a simulated world (see sim.h). It gets no access to
# the library's private headers, so it can only do what an application can. The
# exception is the simulated gateway, which shares the generated wire format.
natpmp_add_test_executable(sim_tests
  client.c
  gateway.c
  main.c
  network.c
  scenarios.c
  sim.c)

target_link_libraries(sim_tests PRIVATE natpmp_warnings)

set_source_files_properties(gateway.c
  PROPERTIES INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/lib)
