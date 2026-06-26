#
# OIL API proving tests.
#
# These tests deliberately exercise OIL through the public API:
#
#   #include <oi/oi.h>
#   oi::setupAndIntrospect(...)
#
# They live inside the main repository build for now, but should avoid relying
# on OI internals unless a test is explicitly investigating an internal failure.
#

set(OIL_API_TEST_LINK_TARGET oil_jit_shared)

set(OIL_API_TEST_CONFIG_PATH
  "${CMAKE_CURRENT_BINARY_DIR}/oil-test.toml"
)

add_custom_command(
  OUTPUT "${OIL_API_TEST_CONFIG_PATH}"
  COMMAND
    ${PYTHON}
    "${PROJECT_SOURCE_DIR}/tools/config_gen.py"
    --compiler "${CMAKE_CXX_COMPILER}"
    "${OIL_API_TEST_CONFIG_PATH}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/config_gen.py"
    "${PROJECT_SOURCE_DIR}/dev.oid.toml"
  WORKING_DIRECTORY
    "${PROJECT_SOURCE_DIR}"
  COMMENT
    "Generating OIL API test config"
  VERBATIM
)

add_custom_target(oil_api_test_config
  DEPENDS "${OIL_API_TEST_CONFIG_PATH}"
)

function(add_oil_api_test target source)
  add_executable(${target}
    ${source}
  )

  add_dependencies(${target}
    oil_api_test_config
  )

  target_compile_definitions(${target}
    PRIVATE
      OIL_TEST_CONFIG_PATH="${OIL_API_TEST_CONFIG_PATH}"
      OIL_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )


  target_link_libraries(${target}
    PRIVATE
      ${OIL_API_TEST_LINK_TARGET}
  )

  target_compile_features(${target}
    PRIVATE
      cxx_std_20
  )

  #
  # OIL depends on debug/type information. Force useful debug output for these
  # proving tests even if the outer build type is not Debug.
  #
  target_compile_options(${target}
    PRIVATE
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-O0>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-g>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-g3>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-omit-frame-pointer>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wextra>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wpedantic>
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
      LABELS "oil"
  )
endfunction()

add_oil_api_test(oil_plain_struct
  oil_plain_struct.cpp
)

add_oil_api_test(oil_nested_struct
  oil_nested_struct.cpp
)

add_oil_api_test(oil_vector_leaf_structs
  oil_vector_leaf_structs.cpp
)

add_oil_api_test(oil_vector_std_string
  oil_vector_std_string.cpp
)

add_oil_api_test(oil_list_leaf_structs
  oil_list_leaf_structs.cpp
)

add_oil_api_test(oil_array_leaf_structs
  oil_array_leaf_structs.cpp
)

add_oil_api_test(oil_rocksdb_core_local_array
  oil_rocksdb_core_local_array.cpp
)
# add_oil_api_test(oil_03_unique_ptr_tree
#   oil_unique_ptr_tree.cpp
# )
#
# add_oil_api_test(oil_04_shared_ptr_graph
#   oil_shared_ptr_graph.cpp
# )
#
# add_oil_api_test(oil_05_vector_of_objects
#   oil_vector_of_objects.cpp
# )
#
# add_oil_api_test(oil_07_map_unordered_map
#   oil_map_unordered_map.cpp
# )
#
# add_oil_api_test(oil_08_variant_optional_tuple
#   oil_variant_optional_tuple.cpp
# )
#
# add_oil_api_test(oil_09_polymorphic_hierarchy
#   oil_polymorphic_hierarchy.cpp
# )
#
# add_oil_api_test(oil_10_cycle
#   oil_cycle.cpp
# )
