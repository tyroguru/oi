#
# reconstruct_demo: a two-process research prototype proving byte-accurate
# object capture on one process and reconstruction on another (see
# ReconstructDemo.cpp's own header comment, and
# docs/object-capture-initial-thoughts.md, not part of this repo).
#
# Built the same way as test/oilgen's AOT tests (compile normally with
# -DOIL_AOT_COMPILATION=1, then run the in-tree oilgen against the same
# source to generate the missing introspectImpl<T>/reconstructImpl<T>
# definitions, then link both object files plus liboil), except this
# target's config additionally enables the capture-bytes feature, which
# reconstruction depends on and the shared test/oilgen config doesn't
# enable.
#

set(RECONSTRUCT_DEMO_BASE_CONFIG_PATH
  "${CMAKE_CURRENT_BINARY_DIR}/reconstruct-demo.oid.toml.base"
)
set(RECONSTRUCT_DEMO_CONFIG_PATH
  "${CMAKE_CURRENT_BINARY_DIR}/reconstruct-demo.oid.toml"
)

add_custom_command(
  OUTPUT "${RECONSTRUCT_DEMO_CONFIG_PATH}"
  COMMAND
    ${PYTHON}
    "${PROJECT_SOURCE_DIR}/tools/config_gen.py"
    --compiler "${CMAKE_CXX_COMPILER}"
    "${RECONSTRUCT_DEMO_BASE_CONFIG_PATH}"
  COMMAND
    ${CMAKE_COMMAND}
    "-DIN_FILE=${RECONSTRUCT_DEMO_BASE_CONFIG_PATH}"
    "-DOUT_FILE=${RECONSTRUCT_DEMO_CONFIG_PATH}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/PrependCaptureBytesFeature.cmake"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/config_gen.py"
    "${PROJECT_SOURCE_DIR}/dev.oid.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/PrependCaptureBytesFeature.cmake"
  WORKING_DIRECTORY
    "${PROJECT_SOURCE_DIR}"
  COMMENT
    "Generating reconstruct_demo oilgen config (capture-bytes enabled)"
  VERBATIM
)

add_custom_target(reconstruct_demo_config
  DEPENDS "${RECONSTRUCT_DEMO_CONFIG_PATH}"
)

# Step 1: compile the source normally.
add_library(reconstruct_demo_obj OBJECT ReconstructDemo.cpp)

target_include_directories(reconstruct_demo_obj
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

target_compile_definitions(reconstruct_demo_obj
  PRIVATE
    OIL_AOT_COMPILATION=1
)

target_compile_features(reconstruct_demo_obj
  PRIVATE
    cxx_std_20
)

target_compile_options(reconstruct_demo_obj
  PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-g>
)

# Step 2: run oilgen against the *same* source to generate the missing
# introspectImpl<DemoObject>/reconstructImpl<DemoObject> definitions.
set(RECONSTRUCT_DEMO_GENERATED_O
  "${CMAKE_CURRENT_BINARY_DIR}/reconstruct_demo.introspection.o"
)

add_custom_command(
  OUTPUT "${RECONSTRUCT_DEMO_GENERATED_O}"
  COMMAND
    $<TARGET_FILE:oilgen>
    -p "${CMAKE_BINARY_DIR}"
    "--config-file=${RECONSTRUCT_DEMO_CONFIG_PATH}"
    "--output=${RECONSTRUCT_DEMO_GENERATED_O}"
    "${CMAKE_CURRENT_SOURCE_DIR}/ReconstructDemo.cpp"
  DEPENDS
    oilgen
    reconstruct_demo_obj
    reconstruct_demo_config
    "${CMAKE_CURRENT_SOURCE_DIR}/ReconstructDemo.cpp"
  COMMENT
    "oilgen: generating AOT introspect+reconstruct code for ReconstructDemo.cpp"
  VERBATIM
)

add_custom_target(reconstruct_demo_generated
  DEPENDS "${RECONSTRUCT_DEMO_GENERATED_O}"
)

# Step 3: link both object files, plus the OIL runtime support library.
add_executable(reconstruct_demo
  $<TARGET_OBJECTS:reconstruct_demo_obj>
  "${RECONSTRUCT_DEMO_GENERATED_O}"
)

add_dependencies(reconstruct_demo reconstruct_demo_generated)

target_link_libraries(reconstruct_demo
  PRIVATE
    oil_shared
)

set_target_properties(reconstruct_demo
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)
