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
# target's config additionally enables the capture-bytes and
# chase-raw-pointers features, which reconstruction depends on and the
# shared test/oilgen config doesn't enable (see
# PrependCaptureBytesFeature.cmake for why chase-raw-pointers is safe to
# turn on here specifically, not as a project-wide default).
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
    "Generating reconstruct_demo oilgen config (capture-bytes, chase-raw-pointers enabled)"
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

#
# cycle_breaking_demo: a standalone, introspect-only proof that BreakCycles
# (facebookexperimental/object-introspection#293 stage 3) lets oilgen
# generate working code for a genuinely self-referential, cyclic raw-pointer
# structure (see CycleBreakingDemo.cpp's own header comment). Built the same
# three-step way as reconstruct_demo above, but its own separate oilgen
# invocation - it can never share a translation unit with reconstruct_demo's
# introspect+reconstruct pair (oilgen only generates for one root per
# invocation), and deliberately never calls oi::reconstruct() at all (see
# CycleBreakingDemo.cpp for why).
#

set(CYCLE_BREAKING_DEMO_BASE_CONFIG_PATH
  "${CMAKE_CURRENT_BINARY_DIR}/cycle-breaking-demo.oid.toml.base"
)
set(CYCLE_BREAKING_DEMO_CONFIG_PATH
  "${CMAKE_CURRENT_BINARY_DIR}/cycle-breaking-demo.oid.toml"
)

add_custom_command(
  OUTPUT "${CYCLE_BREAKING_DEMO_CONFIG_PATH}"
  COMMAND
    ${PYTHON}
    "${PROJECT_SOURCE_DIR}/tools/config_gen.py"
    --compiler "${CMAKE_CXX_COMPILER}"
    "${CYCLE_BREAKING_DEMO_BASE_CONFIG_PATH}"
  COMMAND
    ${CMAKE_COMMAND}
    "-DIN_FILE=${CYCLE_BREAKING_DEMO_BASE_CONFIG_PATH}"
    "-DOUT_FILE=${CYCLE_BREAKING_DEMO_CONFIG_PATH}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/PrependChaseRawPointersFeature.cmake"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/config_gen.py"
    "${PROJECT_SOURCE_DIR}/dev.oid.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/PrependChaseRawPointersFeature.cmake"
  WORKING_DIRECTORY
    "${PROJECT_SOURCE_DIR}"
  COMMENT
    "Generating cycle_breaking_demo oilgen config (chase-raw-pointers enabled)"
  VERBATIM
)

add_custom_target(cycle_breaking_demo_config
  DEPENDS "${CYCLE_BREAKING_DEMO_CONFIG_PATH}"
)

# Step 1: compile the source normally.
add_library(cycle_breaking_demo_obj OBJECT CycleBreakingDemo.cpp)

target_include_directories(cycle_breaking_demo_obj
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

target_compile_definitions(cycle_breaking_demo_obj
  PRIVATE
    OIL_AOT_COMPILATION=1
)

target_compile_features(cycle_breaking_demo_obj
  PRIVATE
    cxx_std_20
)

target_compile_options(cycle_breaking_demo_obj
  PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-g>
)

# Step 2: run oilgen against the *same* source to generate the missing
# introspectImpl<RawNode> definition.
set(CYCLE_BREAKING_DEMO_GENERATED_O
  "${CMAKE_CURRENT_BINARY_DIR}/cycle_breaking_demo.introspection.o"
)

add_custom_command(
  OUTPUT "${CYCLE_BREAKING_DEMO_GENERATED_O}"
  COMMAND
    $<TARGET_FILE:oilgen>
    -p "${CMAKE_BINARY_DIR}"
    "--config-file=${CYCLE_BREAKING_DEMO_CONFIG_PATH}"
    "--output=${CYCLE_BREAKING_DEMO_GENERATED_O}"
    "${CMAKE_CURRENT_SOURCE_DIR}/CycleBreakingDemo.cpp"
  DEPENDS
    oilgen
    cycle_breaking_demo_obj
    cycle_breaking_demo_config
    "${CMAKE_CURRENT_SOURCE_DIR}/CycleBreakingDemo.cpp"
  COMMENT
    "oilgen: generating AOT introspection code for CycleBreakingDemo.cpp"
  VERBATIM
)

add_custom_target(cycle_breaking_demo_generated
  DEPENDS "${CYCLE_BREAKING_DEMO_GENERATED_O}"
)

# Step 3: link both object files, plus the OIL runtime support library.
add_executable(cycle_breaking_demo
  $<TARGET_OBJECTS:cycle_breaking_demo_obj>
  "${CYCLE_BREAKING_DEMO_GENERATED_O}"
)

add_dependencies(cycle_breaking_demo cycle_breaking_demo_generated)

target_link_libraries(cycle_breaking_demo
  PRIVATE
    oil_shared
)

set_target_properties(cycle_breaking_demo
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)
