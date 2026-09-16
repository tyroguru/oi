#
# oilgen AOT proving tests.
#
# Unlike test/oil (which proves the JIT path through `oi::setupAndIntrospect`),
# these tests prove OIL's Ahead-of-Time path: `oilgen` parsing a translation
# unit's Clang AST directly (no drgn/DWARF/JIT involved) and generating the
# `introspectImpl<T>` definition that `oi::introspect<T>()` calls at runtime.
#
# Each test needs two build steps for the same source file:
#   1. Compile it normally, with -DOIL_AOT_COMPILATION=1. This produces an
#      object file that *declares* (but does not define) introspectImpl<T> as
#      a weak symbol - see include/oi/oi.h.
#   2. Run oilgen against that same source file to generate the missing
#      definition as a second object file.
# The two object files are then linked together (plus liboil, for the
# IntrospectionResult/exporters runtime support code) into one executable.
#

set(OILGEN_AOT_TEST_CONFIG_PATH
  "${CMAKE_CURRENT_BINARY_DIR}/oilgen-test.toml"
)

add_custom_command(
  OUTPUT "${OILGEN_AOT_TEST_CONFIG_PATH}"
  COMMAND
    ${PYTHON}
    "${PROJECT_SOURCE_DIR}/tools/config_gen.py"
    --compiler "${CMAKE_CXX_COMPILER}"
    "${OILGEN_AOT_TEST_CONFIG_PATH}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/config_gen.py"
    "${PROJECT_SOURCE_DIR}/dev.oid.toml"
  WORKING_DIRECTORY
    "${PROJECT_SOURCE_DIR}"
  COMMENT
    "Generating oilgen AOT test config"
  VERBATIM
)

add_custom_target(oilgen_aot_test_config
  DEPENDS "${OILGEN_AOT_TEST_CONFIG_PATH}"
)

function(add_oilgen_aot_test target source)
  set(source_path "${CMAKE_CURRENT_SOURCE_DIR}/${source}")

  # Step 1: compile the source normally. This is the "user" side of the
  # example - everything except the introspectImpl<T> definition.
  add_library(${target}_obj OBJECT ${source})

  target_include_directories(${target}_obj
    PRIVATE
      "${PROJECT_SOURCE_DIR}/include"
  )

  target_compile_definitions(${target}_obj
    PRIVATE
      OIL_AOT_COMPILATION=1
  )

  target_compile_features(${target}_obj
    PRIVATE
      cxx_std_20
  )

  target_compile_options(${target}_obj
    PRIVATE
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-g>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wextra>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wpedantic>
  )

  # Step 2: run oilgen against the *same* source file to generate the
  # missing introspectImpl<T> definition. oilgen finds this translation
  # unit's compile flags (include paths, -DOIL_AOT_COMPILATION=1, ...) via
  # the project's compile_commands.json, exactly as an editor/clangd would -
  # it never needs drgn or debug info, only the Clang AST.
  set(generated_o "${CMAKE_CURRENT_BINARY_DIR}/${target}.introspection.o")

  add_custom_command(
    OUTPUT "${generated_o}"
    COMMAND
      $<TARGET_FILE:oilgen>
      -p "${CMAKE_BINARY_DIR}"
      "--config-file=${OILGEN_AOT_TEST_CONFIG_PATH}"
      "--output=${generated_o}"
      "${source_path}"
    DEPENDS
      oilgen
      ${target}_obj
      oilgen_aot_test_config
      "${source_path}"
    COMMENT
      "oilgen: generating AOT introspection code for ${source}"
    VERBATIM
  )

  add_custom_target(${target}_generated
    DEPENDS "${generated_o}"
  )

  # Step 3: link both object files, plus the OIL runtime support library,
  # into one executable.
  add_executable(${target}
    $<TARGET_OBJECTS:${target}_obj>
    "${generated_o}"
  )

  add_dependencies(${target} ${target}_generated)

  target_link_libraries(${target}
    PRIVATE
      oil_shared
  )

  set_target_properties(${target}
    PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
  )

  add_test(
    NAME ${target}
    COMMAND $<TARGET_FILE:${target}>
  )

  set_tests_properties(${target}
    PROPERTIES
      LABELS "oilgen"
  )
endfunction()

add_oilgen_aot_test(oilgen_aot_plain_struct
  AotPlainStruct.cpp
)
