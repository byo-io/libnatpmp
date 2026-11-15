# The library's sources and settings, shared by the library itself and the copy
# the tests use
function(natpmp_add_library target)
  add_library(${target} ${ARGN})

  # The library is C23, its header only needs C11
  target_compile_features(${target}
    PUBLIC c_std_11
    PRIVATE c_std_23)

  target_sources(${target}
    PRIVATE
      gateway.c
      mapping.c
      packet.c
      queue.c
      request.c
      retransmit.c
      session.c
      natpmp_gen.c)

  target_sources(${target}
    PUBLIC
      FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include
        FILES
          ${PROJECT_SOURCE_DIR}/include/byo/natpmp/compat.h
          ${PROJECT_SOURCE_DIR}/include/byo/natpmp/natpmp.h)

  # Only while building, so the installed library doesn't need the warnings
  # target
  target_link_libraries(${target} PRIVATE $<BUILD_INTERFACE:natpmp_warnings>)
endfunction()

natpmp_add_library(natpmp)
add_library(byo::natpmp ALIAS natpmp)

# Installed as libbyo-natpmp, since other projects already use libnatpmp. A
# shared library's soname only changes with the major version. Only functions
# marked with NATPMP_EXPORT are exported.
set_target_properties(natpmp PROPERTIES
  OUTPUT_NAME byo-natpmp
  C_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# The tests check the library's own code with ASan and UBSan too, and the
# library that gets used is never sanitized
if(NATPMP_BUILD_TESTS)
  natpmp_add_library(natpmp_sanitized STATIC)
  target_link_libraries(natpmp_sanitized PUBLIC natpmp_sanitizers)
endif()

# The wire format is generated from natpmp.pkt by pktc
# (https://github.com/davidlwesten/pktc). The output is checked in, so pktc is
# only needed to change the schema. When it's installed, editing the schema
# regenerates the code as part of the build, along with pktc's libFuzzer harness
# for it (test/natpmp_fuzz.c).
find_program(PKTC_EXECUTABLE pktc)
if(PKTC_EXECUTABLE)
  # pktc writes everything into one directory, so generate into the build tree
  # and copy the harness to the tests
  set(PKTC_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/pktc)
  add_custom_command(
    OUTPUT
      ${CMAKE_CURRENT_SOURCE_DIR}/natpmp_gen.c
      ${CMAKE_CURRENT_SOURCE_DIR}/natpmp_gen.h
      ${PROJECT_SOURCE_DIR}/test/natpmp_fuzz.c
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PKTC_OUTPUT}
    COMMAND ${PKTC_EXECUTABLE} --fuzz -o ${PKTC_OUTPUT}
            ${CMAKE_CURRENT_SOURCE_DIR}/natpmp.pkt
    COMMAND ${CMAKE_COMMAND} -E copy ${PKTC_OUTPUT}/natpmp_gen.c
            ${PKTC_OUTPUT}/natpmp_gen.h ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${PKTC_OUTPUT}/natpmp_fuzz.c
            ${PROJECT_SOURCE_DIR}/test
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/natpmp.pkt
    COMMENT "Generating the wire format from natpmp.pkt"
    VERBATIM)
endif()

# Installs the library and its header, and a package so other projects can
# find_package(byo-natpmp) and link to byo::natpmp
if(NATPMP_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  set(NATPMP_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/byo-natpmp)

  install(TARGETS natpmp
    EXPORT byo-natpmpTargets
    FILE_SET HEADERS)
  install(EXPORT byo-natpmpTargets
    NAMESPACE byo::
    DESTINATION ${NATPMP_CMAKE_DIR})

  configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/byo-natpmpConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/byo-natpmpConfig.cmake
    INSTALL_DESTINATION ${NATPMP_CMAKE_DIR})
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/byo-natpmpConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
  install(FILES
      ${CMAKE_CURRENT_BINARY_DIR}/byo-natpmpConfig.cmake
      ${CMAKE_CURRENT_BINARY_DIR}/byo-natpmpConfigVersion.cmake
    DESTINATION ${NATPMP_CMAKE_DIR})
endif()
