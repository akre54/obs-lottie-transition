cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(BUILD_PLUGIN "Build the OBS plugin target" ON)
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

if(BUILD_PLUGIN)
  include(compilerconfig)
  include(defaults)
  include(helpers)

  add_library(${CMAKE_PROJECT_NAME} MODULE)

  find_package(libobs REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

  if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  endif()

  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE src/lottie-transition.c
            src/transform-decode.c
            src/plugin-support.c.in
  )

  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
endif()

include(CTest)

if(BUILD_TESTING)
  add_executable(
    transform-decode-test
    tests/transform-decode-test.c
    src/transform-decode.c
  )

  add_executable(
    transform-decode-cli
    tests/transform-decode-cli.c
    src/transform-decode.c
  )

  target_include_directories(transform-decode-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(transform-decode-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME transform-decode-test COMMAND transform-decode-test)

  find_program(NODE_EXECUTABLE node REQUIRED)
  add_test(
    NAME bridge-core-test
    COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/data/web/test/all.test.js
  )
endif()
