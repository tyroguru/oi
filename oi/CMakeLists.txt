add_library(toml
  support/Toml.cpp
)
target_link_libraries(toml PUBLIC tomlplusplus::tomlplusplus)

add_library(drgn_utils DrgnUtils.cpp)
target_link_libraries(drgn_utils
  glog::glog
  drgn
)

add_library(symbol_service
  Descs.cpp
  SymbolService.cpp
  arch/aarch64.cpp
  arch/x86_64.cpp
)
target_link_libraries(symbol_service
  drgn_utils

  Boost::headers
  ${Boost_LIBRARIES}
  glog::glog

  dw
)

add_library(features Features.cpp)
target_link_libraries(features glog::glog)

add_library(container_info
  ContainerInfo.cpp
)
target_link_libraries(container_info
  features

  Boost::regex
  glog::glog
  toml
)

add_library(codegen
  CodeGen.cpp
  FuncGen.cpp
  OICodeGenConfig.cpp
)
target_link_libraries(codegen
  container_info
  resources
  type_graph

  Boost::headers
  ${Boost_LIBRARIES}
  folly_headers
  glog::glog
)

# The legacy, drgn-based code generator (OICodeGen) and the drgn-only parts
# of CodeGen (CodeGenDrgn.cpp), kept out of `codegen` so that consumers which
# only ever generate code from a Clang AST (i.e. oilgen) don't need to link
# against SymbolService/drgn.
add_library(codegen_drgn
  CodeGenDrgn.cpp
  OICodeGen.cpp
)
target_link_libraries(codegen_drgn
  codegen
  symbol_service
  type_graph_drgn
)

add_library(exporters_csv exporters/CSV.cpp)
target_include_directories(exporters_csv PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(exporters_csv oil)

add_subdirectory(type_graph)
