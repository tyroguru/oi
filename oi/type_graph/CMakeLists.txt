add_library(type_graph
  AddPadding.cpp
  AlignmentCalc.cpp
  ClangTypeParser.cpp
  DetectCycles.cpp
  EnforceCompatibility.cpp
  Flattener.cpp
  IdentifyContainers.cpp
  KeyCapture.cpp
  NameGen.cpp
  PassManager.cpp
  Printer.cpp
  Prune.cpp
  RemoveMembers.cpp
  RemoveTopLevelPointer.cpp
  TopoSorter.cpp
  TypeGraph.cpp
  TypeIdentifier.cpp
  Types.cpp
)
target_link_libraries(type_graph
  container_info
)
target_include_directories(type_graph SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

# The drgn-based type_graph passes (parsing types from DWARF, exporting the
# type graph back to drgn types, and discovering polymorphic children via
# drgn), kept out of `type_graph` so that consumers which only ever build a
# type graph from a Clang AST (e.g. oilgen) don't need to link against
# SymbolService/drgn.
add_library(type_graph_drgn
  AddChildren.cpp
  DrgnExporter.cpp
  DrgnParser.cpp
)
target_link_libraries(type_graph_drgn
  type_graph
  symbol_service
  drgn
)
