#include <gtest/gtest.h>

#include "oi/type_graph/TypeGraph.h"
#include "oi/type_graph/TypeIdentifier.h"
#include "oi/type_graph/Types.h"
#include "test/type_graph_utils.h"

using namespace type_graph;

TEST(TypeIdentifierTest, StubbedParam) {
  test(TypeIdentifier::createPass({}),
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     Struct: MyParam (size: 4)
          Member: a (offset: 0)
            Primitive: int32_t
      Param
        Primitive: int32_t
)",
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[2]     Dummy [MyParam] (size: 4)
      Param
        Primitive: int32_t
)");
}

TEST(TypeIdentifierTest, Allocator) {
  test(TypeIdentifier::createPass({}),
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     Struct: MyAlloc (size: 8)
          Param
            Primitive: int32_t
          Function: allocate
          Function: deallocate
      Param
        Primitive: int32_t
)",
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[2]     DummyAllocator [MyAlloc] (size: 8)
          Primitive: int32_t
      Param
        Primitive: int32_t
)");
}

TEST(TypeIdentifierTest, AllocatorSize1) {
  test(TypeIdentifier::createPass({}),
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     Struct: MyAlloc (size: 1)
          Param
            Primitive: int32_t
          Function: allocate
          Function: deallocate
      Param
        Primitive: int32_t
)",
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[2]     DummyAllocator [MyAlloc] (size: 0)
          Primitive: int32_t
      Param
        Primitive: int32_t
)");
}

TEST(TypeIdentifierTest, PassThroughTypes) {
  std::vector<ContainerInfo> passThroughTypes;
  passThroughTypes.emplace_back("std::allocator", DUMMY_TYPE, "memory");

  test(TypeIdentifier::createPass(passThroughTypes),
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     Class: std::allocator (size: 1)
          Param
            Primitive: int32_t
          Function: allocate
          Function: deallocate
)",
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[2]     Container: std::allocator (size: 1)
          Param
            Primitive: int32_t
          Underlying
[1]         Class: std::allocator (size: 1)
              Param
                Primitive: int32_t
              Function: allocate
              Function: deallocate
)");
}

TEST(TypeIdentifierTest, PassThroughSameType) {
  std::vector<ContainerInfo> passThroughTypes;
  passThroughTypes.emplace_back("std::allocator", DUMMY_TYPE, "memory");

  test(TypeIdentifier::createPass(passThroughTypes),
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     Class: std::allocator (size: 1)
          Param
            Primitive: int32_t
          Function: allocate
          Function: deallocate
      Param
        [1]
)",
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[2]     Container: std::allocator (size: 1)
          Param
            Primitive: int32_t
          Underlying
[1]         Class: std::allocator (size: 1)
              Param
                Primitive: int32_t
              Function: allocate
              Function: deallocate
      Param
        [2]
)");
}

TEST(TypeIdentifierTest, ContainerNotReplaced) {
  test(TypeIdentifier::createPass({}),
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     Container: std::allocator (size: 1)
          Param
            Primitive: int32_t
)",
       R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     Container: std::allocator (size: 1)
          Param
            Primitive: int32_t
)");
}

TEST(TypeIdentifierTest, SmartPointerArrayParamIsIncomplete) {
  ContainerInfo uniquePtrInfo{"std::unique_ptr", UNIQ_PTR_TYPE, "memory"};
  uniquePtrInfo.stubTemplateParams = {1};

  TypeGraph typeGraph;
  auto& element = typeGraph.makeType<Primitive>(Primitive::Kind::Int8);
  auto& array = typeGraph.makeType<Array>(element, 0);
  auto& deleter = typeGraph.makeType<Dummy>(99, 0, 1, "default_delete<char[]>");
  auto& uniquePtr = typeGraph.makeType<Container>(uniquePtrInfo, 8, nullptr);
  uniquePtr.templateParams.emplace_back(array);
  uniquePtr.templateParams.emplace_back(deleter);
  typeGraph.addRoot(uniquePtr);

  NodeTracker tracker;
  auto pass = TypeIdentifier::createPass({});
  pass.run(typeGraph, tracker);

  auto* pointee =
      dynamic_cast<Incomplete*>(&uniquePtr.templateParams[0].type());
  ASSERT_NE(pointee, nullptr);
  ASSERT_TRUE(pointee->underlyingType().has_value());
  EXPECT_NE(dynamic_cast<Array*>(&pointee->underlyingType()->get()), nullptr);
}

TEST(TypeIdentifierTest, DummyNotReplaced) {
  testNoChange(TypeIdentifier::createPass({}), R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     Dummy [InputName] (size: 22)
)");
}

TEST(TypeIdentifierTest, DummyAllocatorNotReplaced) {
  testNoChange(TypeIdentifier::createPass({}), R"(
[0] Container: std::vector (size: 24)
      Param
        Primitive: int32_t
      Param
[1]     DummyAllocator [InputName] (size: 22)
          Primitive: int32_t
)");
}
