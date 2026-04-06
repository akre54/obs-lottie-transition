cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(BUILD_PLUGIN "Build the OBS plugin target" ON)
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(ENABLE_THORVG "Build the experimental ThorVG native backend" ON)

set(LT_MATH_LIB "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(LT_MATH_LIB m)
endif()

if(BUILD_PLUGIN OR BUILD_TESTING)
  add_library(lottie-slot-eval STATIC src/lottie-slot-eval.cpp)
  target_include_directories(lottie-slot-eval PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

if(BUILD_PLUGIN)
  include(compilerconfig)
  include(defaults)
  include(helpers)

  if(ENABLE_THORVG)
    set(THORVG_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/thorvg)
    set(THORVG_BUILD_DIR ${THORVG_SOURCE_DIR}/builddir)
    set(THORVG_PREFIX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/thorvg-install)
    set(THORVG_LIB ${THORVG_PREFIX_DIR}/lib/libthorvg-1.a)

    add_custom_command(
      OUTPUT ${THORVG_LIB}
      COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-thorvg.sh
              ${THORVG_SOURCE_DIR} ${THORVG_BUILD_DIR} ${THORVG_PREFIX_DIR}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-thorvg.sh
              ${THORVG_SOURCE_DIR}/meson.build
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Building vendored ThorVG"
      VERBATIM
    )

    add_custom_target(thorvg_vendor DEPENDS ${THORVG_LIB})
  endif()

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
            src/lottie-backend.c
            src/lottie-thorvg.cpp
            src/transform-decode.c
            src/plugin-support.c.in
  )

  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(
    ${CMAKE_PROJECT_NAME}
    PRIVATE LT_ENABLE_THORVG=$<BOOL:${ENABLE_THORVG}>
            LT_THORVG_RUNTIME_READY=$<BOOL:${ENABLE_THORVG}>
  )

  if(ENABLE_THORVG)
    add_dependencies(${CMAKE_PROJECT_NAME} thorvg_vendor)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${THORVG_PREFIX_DIR}/include/thorvg-1)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${THORVG_LIB})
  endif()

  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE lottie-slot-eval)
  if(LT_MATH_LIB)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${LT_MATH_LIB})
  endif()

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

  add_executable(
    lottie-backend-test
    tests/lottie-backend-test.c
    src/lottie-backend.c
  )

  add_executable(
    lottie-slot-eval-test
    tests/lottie-slot-eval-test.cpp
    src/transform-decode.c
  )

  add_executable(
    lottie-slot-eval-cli
    tests/lottie-slot-eval-cli.cpp
    src/transform-decode.c
  )

  target_include_directories(transform-decode-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(transform-decode-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(lottie-backend-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(lottie-slot-eval-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(lottie-slot-eval-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(
    lottie-backend-test
    PRIVATE LT_ENABLE_THORVG=$<BOOL:${ENABLE_THORVG}>
            LT_THORVG_RUNTIME_READY=$<BOOL:${ENABLE_THORVG}>
  )
  target_compile_definitions(
    lottie-slot-eval-test
    PRIVATE LT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  if(LT_MATH_LIB)
    target_link_libraries(transform-decode-test PRIVATE ${LT_MATH_LIB})
  endif()
  target_link_libraries(lottie-slot-eval-test PRIVATE lottie-slot-eval)
  target_link_libraries(lottie-slot-eval-cli PRIVATE lottie-slot-eval)
  add_test(NAME transform-decode-test COMMAND transform-decode-test)
  add_test(NAME lottie-backend-test COMMAND lottie-backend-test)
  add_test(NAME lottie-slot-eval-test COMMAND lottie-slot-eval-test)

  find_program(NODE_EXECUTABLE node REQUIRED)
  add_test(
    NAME bridge-core-test
    COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/data/web/test/bridge-core.test.js
  )
  add_test(
    NAME backend-plan-test
    COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/data/web/test/backend-plan.test.js
  )
  add_test(
    NAME bridge-examples-test
    COMMAND
      ${CMAKE_COMMAND} -E env
      TRANSFORM_DECODE_CLI=$<TARGET_FILE:transform-decode-cli>
      ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/data/web/test/examples.test.js
  )
  add_test(
    NAME bridge-all-test
    COMMAND
      ${CMAKE_COMMAND} -E env
      TRANSFORM_DECODE_CLI=$<TARGET_FILE:transform-decode-cli>
      LOTTIE_SLOT_EVAL_CLI=$<TARGET_FILE:lottie-slot-eval-cli>
      ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/data/web/test/all.test.js
  )
  add_test(
    NAME native-slot-e2e-test
    COMMAND
      ${CMAKE_COMMAND} -E env
      LOTTIE_SLOT_EVAL_CLI=$<TARGET_FILE:lottie-slot-eval-cli>
      ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/data/web/test/native-slot-e2e.test.js
  )
endif()
