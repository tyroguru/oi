/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "CodeGen.h"

#include <glog/logging.h>

#include <boost/format.hpp>
#include <cassert>
#include <iostream>
#include <numeric>
#include <set>
#include <string_view>

#include "oi/FuncGen.h"
#include "oi/Headers.h"
#include "type_graph/AddPadding.h"
#include "type_graph/AlignmentCalc.h"
#include "type_graph/EnforceCompatibility.h"
#include "type_graph/Flattener.h"
#include "type_graph/IdentifyContainers.h"
#include "type_graph/KeyCapture.h"
#include "type_graph/NameGen.h"
#include "type_graph/Prune.h"
#include "type_graph/RemoveMembers.h"
#include "type_graph/RemoveTopLevelPointer.h"
#include "type_graph/TopoSorter.h"
#include "type_graph/TypeIdentifier.h"
#include "type_graph/Types.h"

template <typename T>
inline constexpr bool always_false_v = false;

namespace oi::detail {

using type_graph::AddPadding;
using type_graph::AlignmentCalc;
using type_graph::CaptureKeys;
using type_graph::Class;
using type_graph::Container;
using type_graph::EnforceCompatibility;
using type_graph::Enum;
using type_graph::Flattener;
using type_graph::IdentifyContainers;
using type_graph::Incomplete;
using type_graph::KeyCapture;
using type_graph::Member;
using type_graph::NameGen;
using type_graph::Primitive;
using type_graph::Prune;
using type_graph::RemoveMembers;
using type_graph::RemoveTopLevelPointer;
using type_graph::TemplateParam;
using type_graph::TopoSorter;
using type_graph::Type;
using type_graph::Typedef;
using type_graph::TypeGraph;
using type_graph::TypeIdentifier;

template <typename T>
using ref = std::reference_wrapper<T>;

namespace {

std::vector<std::string_view> enumerateTypeNames(Type& type) {
  std::vector<std::string_view> names;
  Type* t = &type;

  if (const auto* ck = dynamic_cast<CaptureKeys*>(t))
    t = &ck->underlyingType();

  while (const Typedef* td = dynamic_cast<Typedef*>(t)) {
    names.emplace_back(t->inputName());
    t = &td->underlyingType();
  }
  names.emplace_back(t->inputName());
  return names;
}

void defineMacros(std::string& code) {
  if (true /* TODO: config.useDataSegment*/) {
    code += R"(
#define SAVE_SIZE(val)
#define SAVE_DATA(val)    StoreData(val, returnArg)
)";
  } else {
    code += R"(
#define SAVE_SIZE(val)    AddData(val, returnArg)
#define SAVE_DATA(val)
)";
  }
}

void defineInternalTypes(std::string& code) {
  code += R"(
template<typename T, int N>
struct OIArray {
  T vals[N];
};

// Just here to give a different type name to containers whose keys we'll capture
template <typename T>
struct OICaptureKeys : public T {
};
)";
}

void addPreprocessorDefines(const OICodeGenConfig& config, std::string& code) {
  for (const auto& define : config.preprocessorDefines) {
    const auto equals = define.find('=');
    code += "#define ";
    if (equals == std::string::npos) {
      code += define;
      code += " 1\n";
    } else {
      code += define.substr(0, equals);
      code += ' ';
      code += define.substr(equals + 1);
      code += '\n';
    }
  }
}

void addIncludes(const TypeGraph& typeGraph,
                 const OICodeGenConfig& config,
                 std::string& code) {
  std::set<std::string_view> includes{"cstddef"};
  if (config.features[Feature::TreeBuilderV2]) {
    code += "#define DEFINE_DESCRIBE 1\n";  // added before all includes

    includes.emplace("functional");
    includes.emplace("oi/exporters/inst.h");
    includes.emplace("oi/types/dy.h");
    includes.emplace("oi/types/st.h");
  }
  if (config.features[Feature::Library]) {
    includes.emplace("memory");
    includes.emplace("oi/IntrospectionResult.h");
    includes.emplace("vector");
  }
  if (config.features[Feature::JitTiming]) {
    includes.emplace("chrono");
  }
  if (config.features[Feature::CaptureBytes]) {
    includes.emplace("bit");
  }
  for (const Type& t : typeGraph.finalTypes) {
    if (const auto* c = dynamic_cast<const Container*>(&t)) {
      includes.emplace(c->containerInfo_.header);
    }
  }
  for (const auto& include : includes) {
    code += "#include <";
    code += include;
    code += ">\n";
  }
}

void genDeclsClass(const Class& c, std::string& code) {
  if (c.kind() == Class::Kind::Union)
    code += "union ";
  else
    code += "struct ";
  code += c.name() + ";\n";
}

void genDeclsEnum(const Enum& e, std::string& code) {
  code += "enum class ";
  code += e.name();
  code += " : ";
  switch (e.size()) {
    case 8:
      code += "uint64_t";
      break;
    case 4:
      code += "uint32_t";
      break;
    case 2:
      code += "uint16_t";
      break;
    case 1:
      code += "uint8_t";
      break;
    default:
      abort();  // TODO
  }
  code += " {};\n";
}

void genDecls(const TypeGraph& typeGraph, std::string& code) {
  for (const Type& t : typeGraph.finalTypes) {
    if (const auto* c = dynamic_cast<const Class*>(&t)) {
      genDeclsClass(*c, code);
    } else if (const auto* e = dynamic_cast<const Enum*>(&t)) {
      genDeclsEnum(*e, code);
    }
  }
}

namespace {

size_t calculateExclusiveSize(const Type& t) {
  const Type* finalType = &t;
  while (const auto* td = dynamic_cast<const Typedef*>(finalType)) {
    finalType = &td->underlyingType();
  }

  if (const auto* c = dynamic_cast<const Class*>(finalType)) {
    return std::accumulate(
        c->members.cbegin(), c->members.cend(), 0, [](size_t a, const auto& m) {
          if (m.name.starts_with(AddPadding::MemberPrefix))
            return a + m.type().size();
          return a;
        });
  }
  return finalType->size();
}

}  // namespace

void genNames(const TypeGraph& typeGraph, std::string& code) {
  code += R"(
template <typename T>
struct NameProvider;
)";

  code += R"(
template <unsigned int N, unsigned int align, int32_t Id>
struct NameProvider<DummySizedOperator<N, align, Id>> {
  static constexpr std::array<std::string_view, 0> names = { };
};
)";

  // TODO: stop types being duplicated at this point and remove this check
  std::unordered_set<std::string_view> emittedTypes;
  for (const Type& t : typeGraph.finalTypes) {
    if (dynamic_cast<const Typedef*>(&t))
      continue;
    if (!emittedTypes.emplace(t.name()).second)
      continue;

    code += "template <> struct NameProvider<";
    code += t.name();
    code += "> { static constexpr std::array<std::string_view, 1> names = {\"";
    code += t.inputName();
    code += "\"}; };\n";
  }
}

void genExclusiveSizes(const TypeGraph& typeGraph, std::string& code) {
  code += R"(
template <typename T>
struct ExclusiveSizeProvider {
  static constexpr size_t size = sizeof(T);
};
)";

  for (const Type& t : typeGraph.finalTypes) {
    if (dynamic_cast<const Typedef*>(&t))
      continue;

    size_t exclusiveSize = calculateExclusiveSize(t);
    if (exclusiveSize != t.size()) {
      code += "template <> struct ExclusiveSizeProvider<";
      code += t.name();
      code += "> { static constexpr size_t size = ";
      code += std::to_string(exclusiveSize);
      code += "; };\n";
    }
  }
}

/*
 * Generates a declaration for a given fully-qualified type.
 *
 * e.g. Given "nsA::nsB::Foo"
 *
 * The folowing is generated:
 *   namespace nsA::nsB {
 *   struct Foo;
 *   }  // namespace nsA::nsB
 */
void declareFullyQualifiedStruct(const std::string& name, std::string& code) {
  if (auto pos = name.rfind("::"); pos != name.npos) {
    auto ns = name.substr(0, pos);
    auto structName = name.substr(pos + 2);
    code += "namespace ";
    code += ns;
    code += " {\n";
    code += "struct " + structName + ";\n";
    code += "} // namespace ";
    code += ns;
    code += "\n";
  } else {
    code += "struct ";
    code += name;
    code += ";\n";
  }
}

void genDefsThriftClass(const Class& c, std::string& code) {
  declareFullyQualifiedStruct(c.fqName(), code);
  code += "namespace apache { namespace thrift {\n";
  code += "template <> struct TStructDataStorage<" + c.fqName() + "> {\n";
  code +=
      "  static constexpr const std::size_t fields_size = 1; // Invalid, do "
      "not use\n";
  code +=
      "  static const std::array<folly::StringPiece, fields_size> "
      "fields_names;\n";
  code += "  static const std::array<int16_t, fields_size> fields_ids;\n";
  code +=
      "  static const std::array<protocol::TType, fields_size> fields_types;\n";
  code += "\n";
  code +=
      "  static const std::array<folly::StringPiece, fields_size> "
      "storage_names;\n";
  code +=
      "  static const std::array<int, fields_size> __attribute__((weak)) "
      "isset_indexes;\n";
  code += "};\n";
  code += "}} // namespace thrift, namespace apache\n";
}

}  // namespace

CodeGen::CodeGen(const OICodeGenConfig& config) : config_(config) {
  DCHECK(!config.features[Feature::PolymorphicInheritance])
      << "polymorphic inheritance requires symbol service!";
}

void CodeGen::genDefsThrift(const TypeGraph& typeGraph, std::string& code) {
  for (const Type& t : typeGraph.finalTypes) {
    if (const auto* c = dynamic_cast<const Class*>(&t)) {
      const Member* issetMember = nullptr;
      for (const auto& member : c->members) {
        if (const auto* container =
                dynamic_cast<const Container*>(&member.type());
            container && container->containerInfo_.ctype == THRIFT_ISSET_TYPE) {
          issetMember = &member;
          break;
        }
      }
      if (issetMember) {
        genDefsThriftClass(*c, code);
        thriftIssetMembers_[c] = issetMember;
      }
    }
  }
}

namespace {

void genDefsClass(const Class& c, std::string& code) {
  if (c.kind() == Class::Kind::Union)
    code += "union ";
  else
    code += "struct ";

  if (c.packed()) {
    code += "__attribute__((__packed__)) ";
  }

  if (c.members.size() == 1 &&
      c.members[0].name.starts_with(AddPadding::MemberPrefix)) {
    // Need to specify alignment manually for types which have been stubbed.
    // It would be nice to do this for all types, but our alignment information
    // is not complete, so it would result in some errors.
    //
    // Once we are able to read alignment info from DWARF, then this should be
    // able to be applied to everything.
    code += "alignas(" + std::to_string(c.align()) + ") ";
  }

  code += c.name() + " {\n";
  for (const auto& mem : c.members) {
    code += "  " + mem.type().name() + " " + mem.name;
    if (mem.bitsize) {
      code += " : " + std::to_string(mem.bitsize);
    }
    code += ";\n";
  }
  code += "};\n\n";
}

void genDefsTypedef(const Typedef& td, std::string& code) {
  code += "using " + td.name() + " = " + td.underlyingType().name() + ";\n";
}

void genDefs(const TypeGraph& typeGraph, std::string& code) {
  for (const Type& t : typeGraph.finalTypes) {
    if (const auto* c = dynamic_cast<const Class*>(&t)) {
      genDefsClass(*c, code);
    } else if (const auto* td = dynamic_cast<const Typedef*>(&t)) {
      genDefsTypedef(*td, code);
    }
  }
}

void genStaticAssertsClass(const Class& c, std::string& code) {
  code += "static_assert(validate_size<" + c.name() + ", " +
          std::to_string(c.size()) + ">::value);\n";
  for (const auto& member : c.members) {
    if (member.bitsize > 0)
      continue;

    code += "static_assert(validate_offset<offsetof(" + c.name() + ", " +
            member.name + "), " + std::to_string(member.bitOffset / 8) +
            ">::value, \"Unexpected offset of " + c.name() +
            "::" + member.name + "\");\n";
  }
  code.push_back('\n');
}

void genStaticAssertsContainer(const Container& c, std::string& code) {
  code += "static_assert(validate_size<" + c.name() + ", " +
          std::to_string(c.size()) + ">::value);\n";
  code.push_back('\n');
}

void genStaticAsserts(const TypeGraph& typeGraph, std::string& code) {
  for (const Type& t : typeGraph.finalTypes) {
    if (const auto* c = dynamic_cast<const Class*>(&t)) {
      genStaticAssertsClass(*c, code);
    } else if (const auto* con = dynamic_cast<const Container*>(&t)) {
      genStaticAssertsContainer(*con, code);
    }
  }
}

void addStandardGetSizeFuncDecls(std::string& code) {
  code += R"(
    template <typename T>
    void getSizeType(const T &t, size_t& returnArg);

    template<typename T>
    void getSizeType(/*const*/ T* s_ptr, size_t& returnArg);

    template <typename T, int N>
    void getSizeType(const OIArray<T,N>& container, size_t& returnArg);
  )";
}

void addStandardGetSizeFuncDefs(std::string& code) {
  // TODO use macros, not StoreData directly
  code += R"(
    template <typename T>
    void getSizeType(const T &t, size_t& returnArg) {
      JLOG("obj @");
      JLOGPTR(&t);
      SAVE_SIZE(sizeof(T));
    }
  )";
  // TODO const and non-const versions
  // OR maybe just remove const everywhere
  code += R"(
    template<typename T>
    void getSizeType(/*const*/ T* s_ptr, size_t& returnArg)
    {
      if constexpr (!oi_is_complete<T>) {
        JLOG("incomplete ptr @");
        JLOGPTR(s_ptr);
        StoreData((uintptr_t)(s_ptr), returnArg);
        return;
      } else {
        JLOG("ptr val @");
        JLOGPTR(s_ptr);
        StoreData((uintptr_t)(s_ptr), returnArg);
        if (s_ptr && ctx.pointers.add((uintptr_t)s_ptr)) {
          StoreData(1, returnArg);
          getSizeType(*(s_ptr), returnArg);
        } else {
          StoreData(0, returnArg);
        }
      }
    }

    template <typename T, int N>
    void getSizeType(const OIArray<T,N>& container, size_t& returnArg)
    {
      SAVE_DATA((uintptr_t)N);
      SAVE_SIZE(sizeof(container));

      for (size_t i=0; i<N; i++) {
          // undo the static size that has already been added per-element
          SAVE_SIZE(-sizeof(container.vals[i]));
          getSizeType(container.vals[i], returnArg);
      }
    }
  )";
}

void getClassSizeFuncDecl(const Class& c, std::string& code) {
  code += "void getSizeType(const " + c.name() + " &t, size_t &returnArg);\n";
}
}  // namespace

/*
 * Generates a getSizeType function for the given concrete class.
 *
 * Does not worry about polymorphism.
 */
void CodeGen::getClassSizeFuncConcrete(std::string_view funcName,
                                       const Class& c,
                                       std::string& code) const {
  code += "void " + std::string{funcName} + "(const " + c.name() +
          " &t, size_t &returnArg) {\n";

  const Member* thriftIssetMember = nullptr;
  if (const auto it = thriftIssetMembers_.find(&c);
      it != thriftIssetMembers_.end()) {
    thriftIssetMember = it->second;
  }

  if (thriftIssetMember) {
    code += "  using thrift_data = apache::thrift::TStructDataStorage<" +
            c.fqName() + ">;\n";
  }

  size_t thriftFieldIdx = 0;
  for (size_t i = 0; i < c.members.size(); i++) {
    const auto& member = c.members[i];
    if (member.name.starts_with(AddPadding::MemberPrefix))
      continue;

    if (thriftIssetMember && thriftIssetMember != &member) {
      // Capture Thrift's isset value for each field, except for __isset
      // itself
      std::string issetIdxStr = "thrift_data::isset_indexes[" +
                                std::to_string(thriftFieldIdx++) + "]";
      code += "  if (&thrift_data::isset_indexes != nullptr && " + issetIdxStr +
              " != -1) {\n";
      code += "    SAVE_DATA(t." + thriftIssetMember->name + ".get(" +
              issetIdxStr + "));\n";
      code += "  } else {\n";
      code += "    SAVE_DATA(-1);\n";
      code += "  }\n";
    }

    code += "  JLOG(\"" + member.name + " @\");\n";
    if (member.bitsize == 0)
      code += "  JLOGPTR(&t." + member.name + ");\n";
    code += "  getSizeType(t." + member.name + ", returnArg);\n";
  }
  code += "}\n";
}

void CodeGen::getClassSizeFuncDef(const Class& c, std::string& code) {
  if (!config_.features[Feature::PolymorphicInheritance] || !c.isDynamic()) {
    // Just directly use the concrete size function as this class' getSizeType()
    getClassSizeFuncConcrete("getSizeType", c, code);
    return;
  }

  assert(getClassSizeFuncDefPolymorphicPtr_);
  (this->*getClassSizeFuncDefPolymorphicPtr_)(c, code);
}

namespace {
void getContainerSizeFuncDecl(const Container& c, std::string& code) {
  auto fmt =
      boost::format(c.containerInfo_.codegen.decl) % c.containerInfo_.typeName;
  code += fmt.str();
}

void getContainerSizeFuncDef(std::unordered_set<const ContainerInfo*>& used,
                             const Container& c,
                             std::string& code) {
  if (!used.insert(&c.containerInfo_).second) {
    return;
  }

  auto fmt =
      boost::format(c.containerInfo_.codegen.func) % c.containerInfo_.typeName;
  code += fmt.str();
}

void addGetSizeFuncDecls(const TypeGraph& typeGraph, std::string& code) {
  for (const Type& t : typeGraph.finalTypes) {
    if (const auto* c = dynamic_cast<const Class*>(&t)) {
      getClassSizeFuncDecl(*c, code);
    } else if (const auto* con = dynamic_cast<const Container*>(&t)) {
      getContainerSizeFuncDecl(*con, code);
    }
  }
}

}  // namespace

void CodeGen::addGetSizeFuncDefs(const TypeGraph& typeGraph,
                                 std::string& code) {
  for (const Type& t : typeGraph.finalTypes) {
    if (const auto* c = dynamic_cast<const Class*>(&t)) {
      getClassSizeFuncDef(*c, code);
    } else if (const auto* con = dynamic_cast<const Container*>(&t)) {
      getContainerSizeFuncDef(definedContainers_, *con, code);
    }
  }
}

namespace {

// Find the last member that isn't padding's index. Return -1 if no such member.
size_t getLastNonPaddingMemberIndex(const std::vector<Member>& members) {
  for (size_t i = members.size() - 1; i != (size_t)-1; --i) {
    const auto& el = members[i];
    if (!el.name.starts_with(AddPadding::MemberPrefix))
      return i;
  }
  return -1;
}

}  // namespace

// Generate the function body that walks the type. Uses the monadic
// `delegate()` form to handle each field except for the last. The last field
// instead uses `consume()` as we must not accidentally handle the first half
// of a pair as the last field.
void CodeGen::genClassTraversalFunction(const Class& c, std::string& code) {
  std::string funcName = "getSizeType";

  code += "  static types::st::Unit<DB> ";
  code += funcName;
  code += "(\n      Ctx& ctx,\n";
  code += "    const ";
  code += c.name();
  code += "& t,\n      typename TypeHandler<Ctx, ";
  code += c.name();
  code += ">::type returnArg) {\n";

  const Member* thriftIssetMember = nullptr;
  if (const auto it = thriftIssetMembers_.find(&c);
      it != thriftIssetMembers_.end()) {
    thriftIssetMember = it->second;
  }

  size_t emptySize = code.size();
  size_t lastNonPaddingElement = getLastNonPaddingMemberIndex(c.members);
  size_t thriftFieldIdx = 0;
  for (size_t i = 0; i < lastNonPaddingElement + 1; i++) {
    const auto& member = c.members[i];
    if (member.name.starts_with(AddPadding::MemberPrefix)) {
      continue;
    }

    if (code.size() == emptySize) {
      code += "    return returnArg";
    }

    if (thriftIssetMember != nullptr && thriftIssetMember != &member) {
      code += "\n      .write(getThriftIsset(t, ";
      code += std::to_string(thriftFieldIdx++);
      code += "))";
    }

    code += "\n      .";
    if (i == lastNonPaddingElement) {
      code += "consume";
    } else {
      code += "delegate";
    }
    code +=
        "([&ctx, &t](auto ret) { return OIInternal::getSizeType<Ctx>(ctx, t.";
    code += member.name;
    code += ", ret); })";
  }

  if (code.size() == emptySize) {
    code += "    return returnArg;";
  }
  code += ";\n  }\n";
}

// Generate the static type for the class's representation in the data buffer.
// For `class { int a,b,c; }` we generate (Ctx/DB omitted for clarity):
// Pair<TypeHandler<int>::type,
//   Pair<TypeHandler<int>::type,
//     TypeHandler<int>::type
// >>
void CodeGen::genClassStaticType(const Class& c, std::string& code) {
  const Member* thriftIssetMember = nullptr;
  if (const auto it = thriftIssetMembers_.find(&c);
      it != thriftIssetMembers_.end()) {
    thriftIssetMember = it->second;
  }

  size_t lastNonPaddingElement = getLastNonPaddingMemberIndex(c.members);
  size_t pairs = 0;

  size_t emptySize = code.size();
  for (size_t i = 0; i < lastNonPaddingElement + 1; i++) {
    const auto& member = c.members[i];
    if (member.name.starts_with(AddPadding::MemberPrefix)) {
      continue;
    }

    if (i != lastNonPaddingElement) {
      code += "types::st::Pair<DB, ";
      pairs++;
    }

    if (thriftIssetMember != nullptr && thriftIssetMember != &member) {
      // Return an additional VarInt before every field except for __isset
      // itself.
      pairs++;
      if (i == lastNonPaddingElement) {
        code += "types::st::Pair<DB, types::st::VarInt<DB>, ";
      } else {
        code += "types::st::VarInt<DB>, types::st::Pair<DB, ";
      }
    }

    code +=
        (boost::format("typename TypeHandler<Ctx, decltype(%1%::%2%)>::type") %
         c.name() % member.name)
            .str();

    if (i != lastNonPaddingElement) {
      code += ", ";
    }
  }
  code += std::string(pairs, '>');

  if (code.size() == emptySize) {
    code += "types::st::Unit<DB>";
  }
}

void CodeGen::genClassTreeBuilderInstructions(const Class& c,
                                              std::string& code) {
  const Member* thriftIssetMember = nullptr;
  if (const auto it = thriftIssetMembers_.find(&c);
      it != thriftIssetMembers_.end()) {
    thriftIssetMember = it->second;
  }

  code += " private:\n";
  size_t index = 0;
  for (const auto& m : c.members) {
    ++index;
    if (m.name.starts_with(AddPadding::MemberPrefix))
      continue;

    auto names = enumerateTypeNames(m.type());
    code += "  static constexpr std::array<std::string_view, " +
            std::to_string(names.size()) + "> member_" + std::to_string(index) +
            "_type_names = {";
    for (const auto& name : names) {
      code += "\"";
      code += name;
      code += "\",";
    }
    code += "};\n";
  }

  code += " public:\n";
  size_t numFields =
      std::count_if(c.members.cbegin(), c.members.cend(), [](const auto& m) {
        return !m.name.starts_with(AddPadding::MemberPrefix);
      });
  code += "  static constexpr std::array<inst::Field, ";
  code += std::to_string(numFields);
  code += "> fields{\n";
  index = 0;

  for (const auto& m : c.members) {
    ++index;
    if (m.name.starts_with(AddPadding::MemberPrefix))
      continue;
    std::string fullName = c.name() + "::" + m.name;
    bool isbitField = m.bitsize;
    bool isPrimitive = dynamic_cast<const Primitive*>(&m.type());

    if (!isbitField) {
      code += "      inst::Field{sizeof(";
      code += fullName;
      code += "), ";
      code += std::to_string(calculateExclusiveSize(m.type()));
    } else {
      code += "      inst::Field{0, 0";
    }
    code += ", \"";
    code += m.inputName;
    code += "\", member_";
    code += std::to_string(index);
    code += "_type_names, TypeHandler<Ctx, decltype(";
    code += fullName;
    code += ")>::fields, ";

    if (thriftIssetMember != nullptr && thriftIssetMember != &m) {
      code += "ThriftIssetHandler<";
    }
    code += "TypeHandler<Ctx, decltype(";
    code += fullName;
    code += ")>";
    if (thriftIssetMember != nullptr && thriftIssetMember != &m) {
      code += '>';
    }
    code += "::processors, ";

    code += isPrimitive ? "true" : "false";
    code += "},\n";
  }
  code += "  };\n";
  code +=
      "static constexpr std::array<exporters::inst::ProcessorInst, 0> "
      "processors{};\n";
}

void CodeGen::genClassTypeHandler(const Class& c, std::string& code) {
  std::string helpers;

  if (const auto it = thriftIssetMembers_.find(&c);
      it != thriftIssetMembers_.end()) {
    const Member& thriftIssetMember = *it->second;

    helpers += (boost::format(R"(
  static int getThriftIsset(const %1%& t, size_t i) {
    using thrift_data = apache::thrift::TStructDataStorage<%2%>;

    if (&thrift_data::isset_indexes == nullptr) return 2;

    auto idx = thrift_data::isset_indexes[i];
    if (idx == -1) return 2;

    return t.%3%.get(idx);
  }
)") % c.name() % c.fqName() %
                thriftIssetMember.name)
                   .str();
  }

  code += "template <typename Ctx>\n";
  code += "class TypeHandler<Ctx, ";
  code += c.name();
  code += "> {\n";
  code += "  using DB = typename Ctx::DataBuffer;\n";
  code += helpers;
  code += " public:\n";
  code += "  using type = ";
  genClassStaticType(c, code);
  code += ";\n";
  genClassTreeBuilderInstructions(c, code);
  genClassTraversalFunction(c, code);
  code += "};\n";
}

namespace {

void genContainerTypeHandler(std::unordered_set<const ContainerInfo*>& used,
                             const ContainerInfo& c,
                             std::span<const TemplateParam> templateParams,
                             std::string& code) {
  if (!used.insert(&c).second)
    return;

  code += c.codegen.extra;

  // TODO: Move this check into the ContainerInfo parsing once always enabled.
  const auto& func = c.codegen.traversalFunc;
  const auto& processors = c.codegen.processors;

  if (func.empty()) {
    LOG(ERROR)
        << "`codegen.traversal_func` must be specified for all containers "
           "under \"-ftree-builder-v2\", not specified for \"" +
               c.typeName + "\"";
    throw std::runtime_error("missing `codegen.traversal_func`");
  }

  std::string containerWithTypes = c.typeName;
  if (!templateParams.empty())
    containerWithTypes += '<';
  size_t types = 0, values = 0;
  for (const auto& p : templateParams) {
    if (types > 0 || values > 0)
      containerWithTypes += ", ";
    if (p.value) {
      containerWithTypes += "N" + std::to_string(values++);
    } else {
      containerWithTypes += "T" + std::to_string(types++);
    }
  }
  if (!templateParams.empty())
    containerWithTypes += '>';

  if (c.captureKeys) {
    containerWithTypes = "OICaptureKeys<" + containerWithTypes + ">";
  }

  // TODO: This is tech debt. This should be moved to a field in the container
  // spec/`.toml` called something like `codegen.handler_header` or have an
  // explicit option for variable template parameters. However I'm landing it
  // anyway to demonstrate how to handle tagged unions in TreeBuilder-v2.
  if (c.typeName == "std::variant") {
    code += R"(
template <typename Ctx, typename... Types>
struct TypeHandler<Ctx, std::variant<Types...>> {
  using container_type = std::variant<Types...>;
)";
  } else {
    code += "template <typename Ctx";
    types = 0, values = 0;
    for (const auto& p : templateParams) {
      if (p.value) {
        code += ", ";

        // HACK: forward all enums directly. this might turn out to be a problem
        // if there are enums we should be regenerating/use in the body.
        if (const auto* e = dynamic_cast<const Enum*>(&p.type())) {
          code += e->inputName();
        } else {
          code += p.type().name();
        }

        code += " N" + std::to_string(values++);
      } else {
        code += ", typename T" + std::to_string(types++);
      }
    }
    code += ">\n";
    code += "struct TypeHandler<Ctx, ";
    code += containerWithTypes;
    code += "> {\n";
    code += "  using container_type = ";
    code += containerWithTypes;
    code += ";\n";
  }

  code += "  using DB = typename Ctx::DataBuffer;\n";

  if (c.captureKeys) {
    code += "  static constexpr bool captureKeys = true;\n";
  } else {
    code += "  static constexpr bool captureKeys = false;\n";
  }

  code += "  using type = ";
  if (processors.empty()) {
    code += "types::st::Unit<DB>";
  } else {
    for (auto it = processors.cbegin(); it != processors.cend(); ++it) {
      if (it != processors.cend() - 1)
        code += "types::st::Pair<DB, ";
      code += it->type;
      if (it != processors.cend() - 1)
        code += ", ";
    }
    code += std::string(processors.size() - 1, '>');
  }
  code += ";\n";

  code += c.codegen.scopedExtra;

  code += "  static types::st::Unit<DB> getSizeType(\n";
  code += "      Ctx& ctx,\n";
  code += "      const container_type& container,\n";
  code +=
      "      typename TypeHandler<Ctx, container_type>::type returnArg) {\n";
  code += func;  // has rubbish indentation
  code += "  }\n";

  code += " private:\n";
  size_t count = 0;
  for (const auto& pr : processors) {
    code += "  static void processor_";
    code += std::to_string(count++);
    code +=
        "(result::Element& el, std::function<void(inst::Inst)> stack_ins, "
        "ParsedData d) {\n";
    code += pr.func;  // bad indentation
    code += "  }\n";
  }

  code += " public:\n";
  code +=
      "  static constexpr std::array<exporters::inst::Field, 0> fields{};\n";
  code += "  static constexpr std::array<exporters::inst::ProcessorInst, ";
  code += std::to_string(processors.size());
  code += "> processors{\n";
  count = 0;
  for (const auto& pr : processors) {
    code += "    exporters::inst::ProcessorInst{";
    code += pr.type;
    code += "::describe, &processor_";
    code += std::to_string(count++);
    code += "},\n";
  }
  code += "  };\n";
  code += "};\n\n";
}

void addCaptureKeySupport(std::string& code) {
  code += R"(
    template <typename Ctx, typename T>
    class CaptureKeyHandler {
      using DB = typename Ctx::DataBuffer;
     public:
      using type = types::st::Sum<DB, types::st::VarInt<DB>, types::st::VarInt<DB>>;

      static auto captureKey(const T& key, auto returnArg) {
        // Save scalars keys directly, otherwise save pointers for complex types
        if constexpr (std::is_scalar_v<T>) {
          return returnArg.template write<0>().write(static_cast<uint64_t>(key));
        }
        return returnArg.template write<1>().write(reinterpret_cast<uintptr_t>(&key));
      }
    };

    template <bool CaptureKeys, typename Ctx, typename T>
    auto maybeCaptureKey(Ctx& ctx, const T& key, auto returnArg) {
      if constexpr (CaptureKeys) {
        return returnArg.delegate([&key](auto ret) {
          return CaptureKeyHandler<Ctx, T>::captureKey(key, ret);
        });
      } else {
        return returnArg;
      }
    }

    template <typename Ctx, typename T>
    static constexpr inst::ProcessorInst CaptureKeysProcessor{
      CaptureKeyHandler<Ctx, T>::type::describe,
      [](result::Element& el, std::function<void(inst::Inst)> stack_ins, ParsedData d) {
        if constexpr (std::is_same_v<
            typename CaptureKeyHandler<Ctx, T>::type,
            types::st::List<typename Ctx::DataBuffer, types::st::VarInt<typename Ctx::DataBuffer>>>) {
          // String
          auto& str = el.data.emplace<std::string>();
          auto list = std::get<ParsedData::List>(d.val);
          size_t strlen = list.length;
          for (size_t i = 0; i < strlen; i++) {
            auto value = list.values().val;
            auto c = std::get<ParsedData::VarInt>(value).value;
            str.push_back(c);
          }
        } else {
          auto sum = std::get<ParsedData::Sum>(d.val);
          if (sum.index == 0) {
            el.data = oi::result::Element::Scalar{std::get<ParsedData::VarInt>(sum.value().val).value};
          } else {
            el.data = oi::result::Element::Pointer{std::get<ParsedData::VarInt>(sum.value().val).value};
          }
        }
      }
    };

    template <bool CaptureKeys, typename Ctx, typename T>
    static constexpr auto maybeCaptureKeysProcessor() {
      if constexpr (CaptureKeys) {
        return std::array<inst::ProcessorInst, 1>{
          CaptureKeysProcessor<Ctx, T>,
        };
      }
      else {
        return std::array<inst::ProcessorInst, 0>{};
      }
    }
  )";
}

void addCaptureBytesSupport(std::string& code) {
  code += R"(
    // Conditionally writes a runtime-length raw byte span (e.g. a string's
    // content) instead of nothing. Used by container definitions whose
    // traversal_func has its own content to offer beyond what the generic
    // per-element TypeHandler dispatch reaches (std::string and friends
    // don't walk per-character). returnArg's type is already exactly
    // DynBytes<DB> (CaptureBytes on) or Unit<DB> (off), via the same
    // std::conditional_t<oi_capture_bytes, ...> pattern captureKeys uses
    // for its own processor type - no delegation needed, just a direct
    // write or a pass-through.
    template <bool CaptureBytes>
    auto maybeCaptureBytes(auto returnArg, std::span<const uint8_t> bytes) {
      if constexpr (CaptureBytes) {
        return returnArg.write(bytes);
      } else {
        return returnArg;
      }
    }
  )";
}

void addThriftIssetSupport(std::string& code) {
  code += R"(
void processThriftIsset(result::Element& el, std::function<void(inst::Inst)> stack_ins, ParsedData d) {
  auto v = std::get<ParsedData::VarInt>(d.val).value;
  if (v <= 1) {
    el.is_set_stats.emplace(result::Element::IsSetStats { v == 1 });
  }
}
static constexpr exporters::inst::ProcessorInst thriftIssetProcessor{
  types::st::VarInt<int>::describe,
  &processThriftIsset,
};

template <typename Handler>
struct ThriftIssetHandler {
  static constexpr auto processors = arrayPrepend(Handler::processors, thriftIssetProcessor);
};
)";
}

void addStandardTypeHandlers(TypeGraph& typeGraph,
                             FeatureSet features,
                             std::string& code) {
  addCaptureKeySupport(code);
  addCaptureBytesSupport(code);
  if (features[Feature::CaptureThriftIsset])
    addThriftIssetSupport(code);

  // Provide a wrapper function, getSizeType, to infer T instead of having to
  // explicitly specify it with TypeHandler<Ctx, T>::getSizeType every time.
  code += R"(
    template <typename Ctx, typename T>
    types::st::Unit<typename Ctx::DataBuffer>
    getSizeType(Ctx& ctx, const T &t, typename TypeHandler<Ctx, T>::type returnArg) {
      JLOG("obj @");
      JLOGPTR(&t);
      return TypeHandler<Ctx, T>::getSizeType(ctx, t, returnArg);
    }
)";

  // TODO: bit of a hack - making ContainerInfo a node in the type graph and
  // traversing for it would remove the need for this set altogether.
  std::unordered_set<const ContainerInfo*> used{};
  std::vector<TemplateParam> arrayParams{
      TemplateParam{typeGraph.makeType<Primitive>(Primitive::Kind::UInt64)},
      TemplateParam{typeGraph.makeType<Primitive>(Primitive::Kind::UInt64),
                    "0"},
  };
  genContainerTypeHandler(
      used, FuncGen::GetOiArrayContainerInfo(), arrayParams, code);
}

}  // namespace

void CodeGen::addTypeHandlers(const TypeGraph& typeGraph, std::string& code) {
  for (const Type& t : typeGraph.finalTypes) {
    if (const auto* c = dynamic_cast<const Class*>(&t)) {
      genClassTypeHandler(*c, code);
    } else if (const auto* con = dynamic_cast<const Container*>(&t)) {
      genContainerTypeHandler(
          definedContainers_, con->containerInfo_, con->templateParams, code);
    } else if (const auto* cap = dynamic_cast<const CaptureKeys*>(&t)) {
      auto* container =
          dynamic_cast<Container*>(&(stripTypedefs(cap->underlyingType())));
      if (!container)
        throw std::runtime_error("KaptureKeys requires a container");

      genContainerTypeHandler(definedContainers_,
                              cap->containerInfo(),
                              container->templateParams,
                              code);
    }
  }
}

bool CodeGen::registerContainers() {
  try {
    containerInfos_.reserve(config_.containerConfigPaths.size());
    for (const auto& path : config_.containerConfigPaths) {
      registerContainer(path);
    }
    return true;
  } catch (const ContainerInfoError& err) {
    LOG(ERROR) << "Error reading container TOML file " << err.what();
    return false;
  }
}

void CodeGen::registerContainer(std::unique_ptr<ContainerInfo> info) {
  VLOG(1) << "Registered container: " << info->typeName;
  containerInfos_.emplace_back(std::move(info));
}

void CodeGen::registerContainer(const fs::path& path) {
  auto info = std::make_unique<ContainerInfo>(path);
  if (info->requiredFeatures != (config_.features & info->requiredFeatures)) {
    VLOG(1) << "Skipping container (feature conflict): " << info->typeName;
    return;
  }
  registerContainer(std::move(info));
}

void CodeGen::transform(TypeGraph& typeGraph) {
  type_graph::PassManager pm;

  // Simplify the type graph first so there is less work for later passes
  pm.addPass(RemoveTopLevelPointer::createPass());
  pm.addPass(IdentifyContainers::createPass(containerInfos_));
  pm.addPass(Flattener::createPass());
  pm.addPass(AlignmentCalc::createPass());
  pm.addPass(TypeIdentifier::createPass(config_.passThroughTypes));
  if (config_.features[Feature::PruneTypeGraph])
    pm.addPass(Prune::createPass());

  if (config_.features[Feature::PolymorphicInheritance]) {
    assert(addPolymorphicInheritanceChildrenPtr_);
    (this->*addPolymorphicInheritanceChildrenPtr_)(pm, typeGraph);
  }

  pm.addPass(RemoveMembers::createPass(config_.membersToStub));
  if (!config_.features[Feature::TreeBuilderV2])
    pm.addPass(EnforceCompatibility::createPass());
  if (config_.features[Feature::TreeBuilderV2] &&
      !config_.keysToCapture.empty())
    pm.addPass(KeyCapture::createPass(config_.keysToCapture, containerInfos_));

  // Add padding to fill in the gaps of removed members and ensure their
  // alignments
  pm.addPass(AddPadding::createPass());

  pm.addPass(NameGen::createPass());
  pm.addPass(TopoSorter::createPass());

  pm.run(typeGraph);

  VLOG(1) << "Sorted types:\n";
  for (Type& t : typeGraph.finalTypes) {
    VLOG(1) << "  " << t.name() << std::endl;
  };
}

void CodeGen::generate(TypeGraph& typeGraph,
                       std::string& code,
                       RootFunctionName rootName) {
  code.clear();
  addPreprocessorDefines(config_, code);
  code += headers::oi_OITraceCode_cpp;
  if (!config_.features[Feature::Library]) {
    FuncGen::DeclareExterns(code);
  }
  if (!config_.features[Feature::TreeBuilderV2]) {
    defineMacros(code);
  }
  addIncludes(typeGraph, config_, code);
  defineInternalTypes(code);
  FuncGen::DefineJitLog(code, config_.features);

  if (config_.features[Feature::TreeBuilderV2]) {
    if (config_.features[Feature::Library]) {
      FuncGen::DefineBackInserterDataBuffer(code);
    } else {
      FuncGen::DefineDataSegmentDataBuffer(code);
    }
    code += "using namespace oi;\n";
    code += "using namespace oi::detail;\n";
    code += "using oi::exporters::ParsedData;\n";
    code += "using namespace oi::exporters;\n";
  }

  if (config_.features[Feature::CaptureThriftIsset]) {
    genDefsThrift(typeGraph, code);
  }
  if (!config_.features[Feature::TreeBuilderV2]) {
    code += "namespace {\n";
    code += "static struct Context {\n";
    code += "  PointerHashSet<> pointers;\n";
    code += "} ctx;\n";
    code += "} // namespace\n";
  }

  /*
   * The purpose of the anonymous namespace within `OIInternal` is that
   * anything defined within an anonymous namespace has internal-linkage,
   * and therefore won't appear in the symbol table of the resulting object
   * file. Both OIL and OID do a linear search through the symbol table for
   * the top-level `getSize` function to locate the probe entry point, so
   * by keeping the contents of the symbol table to a minimum, we make that
   * process faster.
   */
  code += "namespace OIInternal {\nnamespace {\n";
  if (!config_.features[Feature::TreeBuilderV2]) {
    FuncGen::DefineEncodeData(code);
    FuncGen::DefineEncodeDataSize(code);
    FuncGen::DefineStoreData(code);
  }
  FuncGen::DeclareGetContainer(code);

  genDecls(typeGraph, code);
  genDefs(typeGraph, code);
  genStaticAsserts(typeGraph, code);
  if (config_.features[Feature::TreeBuilderV2]) {
    genNames(typeGraph, code);
    genExclusiveSizes(typeGraph, code);
  }

  if (config_.features[Feature::TreeBuilderV2]) {
    FuncGen::DefineBasicTypeHandlers(code, config_.features);
    addStandardTypeHandlers(typeGraph, config_.features, code);
    addTypeHandlers(typeGraph, code);
  } else {
    addStandardGetSizeFuncDecls(code);
    addGetSizeFuncDecls(typeGraph, code);

    addStandardGetSizeFuncDefs(code);
    addGetSizeFuncDefs(typeGraph, code);
  }

  assert(typeGraph.rootTypes().size() == 1);
  Type& rootType = typeGraph.rootTypes()[0];
  code += "\nusing __ROOT_TYPE__ = " + rootType.name() + ";\n";
  code += "} // namespace\n} // namespace OIInternal\n";

  const auto& typeToHash = std::visit(
      [](const auto& v) -> const std::string& {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<ExactName, T> ||
                      std::is_same_v<HashedComponent, T>) {
          return v.name;
        } else {
          static_assert(always_false_v<T>, "missing visit");
        }
      },
      rootName);

  if (config_.features[Feature::TreeBuilderV2]) {
    FuncGen::DefineTopLevelIntrospect(code, typeToHash);
  } else {
    FuncGen::DefineTopLevelGetSizeRef(code, typeToHash, config_.features);
  }

  if (config_.features[Feature::TreeBuilderV2]) {
    FuncGen::DefineTreeBuilderInstructions(code,
                                           typeToHash,
                                           calculateExclusiveSize(rootType),
                                           enumerateTypeNames(rootType));
  }

  if (auto* n = std::get_if<ExactName>(&rootName))
    FuncGen::DefineTopLevelIntrospectNamed(code, typeToHash, n->name);

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "Generated trace code:\n";
    // VLOG truncates output, so use std::cerr
    std::cerr << code;
  }
}

namespace {

// Unwraps Typedefs to find the underlying type a type resolves to -
// reconstruction only knows how to rebuild scalars and containers (not,
// say, a Typedef node itself), whether reconstructing a class's member
// (see CodeGen::collectReconstructableMembers) or a container's element
// (see CodeGen::emitReconstructValue).
Type& unwrapTypedefs(Type& t) {
  Type* resolved = &t;
  while (auto* td = dynamic_cast<Typedef*>(resolved))
    resolved = &td->underlyingType();
  return *resolved;
}

// Recursively names the concrete C++ type reconstruction should produce
// for `t` - a Primitive's own name, or a reconstructable Container
// parameterized by its own element type's name in turn (e.g.
// "std::vector<std::__cxx11::basic_string<char>>"). Needed because
// Container::name() alone is just the bare container name with no
// template arguments (e.g. "std::vector") - not enough to name a
// concrete, constructible type on its own. See generateReconstructContainerBody's
// t0Name comment for the known char/int8_t caveat this inherits.
std::string resolveTypeName(Type& t) {
  Type& resolved = unwrapTypedefs(t);

  if (auto* prim = dynamic_cast<Primitive*>(&resolved))
    return prim->name();

  // Enums are re-declared as flat, independent `enum class Name : uintN_t
  // {};` types by genDeclsEnum (see genDecls' dispatch) whenever a class
  // root's own preamble runs genDecls, the same treatment a nested C++
  // enum (e.g. Product::ProductType::Enum) gets regardless of its
  // original nesting. Unlike a Primitive's builtin spelling (globally
  // visible everywhere) or a Container's std::-qualified spelling, this
  // declaration lands inside `namespace OIInternal { namespace {...} }` -
  // and the reconstructImpl<T> function body that names this type is
  // emitted at global scope (see appendReconstructFunctionBody), outside
  // that namespace - so the name must be qualified to remain resolvable
  // there.
  if (auto* en = dynamic_cast<Enum*>(&resolved))
    return "OIInternal::" + en->name();

  if (auto* cont = dynamic_cast<Container*>(&resolved)) {
    if (cont->templateParams.empty())
      throw std::runtime_error("CodeGen::resolveTypeName: " +
                               cont->containerInfo_.typeName +
                               " has no template parameters");
    std::string name = cont->containerInfo_.typeName + "<" +
                       resolveTypeName(cont->templateParams[0].type());
    // A second template parameter (e.g. a map's value type) is named too,
    // when present - a single-param name like "std::map<K>" wouldn't even
    // be a valid type.
    if (cont->templateParams.size() >= 2)
      name += ", " + resolveTypeName(cont->templateParams[1].type());
    return name + ">";
  }

  throw std::runtime_error(
      "CodeGen::resolveTypeName: " + resolved.name() +
      " is neither a scalar, an enum, nor a container - cannot name its "
      "reconstructed type");
}

struct ReconstructableMember {
  std::string_view name;
  Type* type;  // already unwrapped past any Typedef - Primitive or Container
};

}  // namespace

void CodeGen::generateReconstructScalar(Primitive& primitive,
                                        const std::string& typeToHash,
                                        std::string& code) {
  const std::string& typeName = primitive.name();

  code += "extern \"C\" " + typeName + " " + typeToHash +
          "(std::span<const uint8_t> bytes) {\n";
  code += "  std::vector<uint8_t> vec(bytes.begin(), bytes.end());\n";
  code += "  auto it = vec.cbegin();\n";
  code += "  oi::types::dy::Bytes shape{sizeof(" + typeName + ")};\n";
  code += "  auto parsed = oi::exporters::ParsedData::parse(it, shape);\n";
  code += "  return oi::exporters::reconstructScalar<" + typeName +
          ">(std::get<oi::exporters::ParsedData::Bytes>(parsed.val));\n";
  code += "}\n";
}

namespace {

// Shared by generateReconstructClassBody (standalone reconstruct-only
// codegen) and appendReconstructFunctionBody's class path (combined
// introspect+reconstruct codegen, see CodeGen.h) - both need the same
// "which members can we rebuild, and under what name" answer, independent
// of whether the struct's own OIInternal redeclaration is being emitted
// alongside them or was already emitted by generate() for the same root.
std::vector<ReconstructableMember> collectReconstructableMembers(
    const Class& cls) {
  size_t lastNonPaddingElement = getLastNonPaddingMemberIndex(cls.members);
  if (lastNonPaddingElement == (size_t)-1) {
    throw std::runtime_error(
        "CodeGen::generateReconstructClass: " + cls.name() +
        " has no non-padding members - not yet supported");
  }

  std::vector<ReconstructableMember> members;
  for (size_t i = 0; i < lastNonPaddingElement + 1; i++) {
    const auto& member = cls.members[i];
    if (member.name.starts_with(AddPadding::MemberPrefix))
      continue;

    Type& resolved = unwrapTypedefs(member.type());
    if (!dynamic_cast<Primitive*>(&resolved) &&
        !dynamic_cast<Enum*>(&resolved) &&
        !dynamic_cast<Container*>(&resolved)) {
      throw std::runtime_error(
          "CodeGen::generateReconstructClass: member " + cls.name() + "::" +
          member.name +
          " is neither a scalar, an enum, nor a reconstructable container "
          "- not yet supported (see docs/object-capture-initial-thoughts.md "
          "- pointer fixup and nested class members are not yet "
          "implemented)");
    }

    members.push_back({.name = member.name, .type = &resolved});
  }
  return members;
}

}  // namespace

// Emits, inside namespace OIInternal { namespace {...} }, everything
// generateReconstructClassBody's/emitReconstructValue's use of
// TypeHandler<Ctx, T>::type::describe needs to exist: the generic
// scalar/pointer TypeHandler (FuncGen::DefineBasicTypeHandlers) plus a
// real specialization for every container reachable from this root
// (addStandardTypeHandlers/addTypeHandlers, driven off
// typeGraph.finalTypes - already populated, since transform() runs
// unconditionally before either codegen path). Shared between the
// container root case (generateReconstruct(), which calls this directly)
// and the class root case (generateReconstructClassPreamble, below) -
// only the *standalone* paths need it at all, since generate() already
// does the equivalent for the combined introspect+reconstruct case.
//
// Must run after the struct itself is declared/defined (when there is
// one) - addTypeHandlers emits its own TypeHandler<Ctx, ThisClass>
// specialization too (harmless but requires the class name to already
// exist), which is why generateReconstructClassPreamble calls this after
// genDefs, not before.
void CodeGen::emitReconstructTypeHandlerSupport(TypeGraph& typeGraph,
                                                std::string& code) {
  // DefineBasicTypeHandlers' make_field<Ctx,T>() (never instantiated by
  // this path, but still parsed as part of the template definition)
  // references these dependent names, which two-phase lookup needs
  // declared as templates regardless. ExclusiveSizeProvider needs its
  // real generic definition (make_field's own use is unconditional, not
  // gated behind an `if constexpr`); NameProvider only needs the forward
  // declaration genNames() itself starts from - a per-type specialization
  // is never required since make_field is never actually called here.
  code += "template <typename T> struct NameProvider;\n";
  code +=
      "template <typename T> struct ExclusiveSizeProvider { static "
      "constexpr size_t size = sizeof(T); };\n";
  FuncGen::DefineBasicTypeHandlers(code, config_.features);
  addStandardTypeHandlers(typeGraph, config_.features, code);
  addTypeHandlers(typeGraph, code);
}

// Emits the struct's own redeclaration (internal-linkage, layout-identical
// to the real type - see generate()'s identical trick for introspectImpl)
// and the OIInternal::__ROOT_TYPE__ alias the body below depends on. Split
// out from generateReconstructClassBody so appendReconstructFunctionBody
// can skip this entirely when generate() already emitted an equivalent
// redeclaration for the same root (the combined introspect+reconstruct
// case - see CodeGen.h).
void CodeGen::generateReconstructClassPreamble(TypeGraph& typeGraph,
                                               Class& cls,
                                               std::string& code) {
  code += "namespace OIInternal {\nnamespace {\n";
  defineInternalTypes(code);  // OIArray<>, used by padding members below
  genDecls(typeGraph, code);
  genDefs(typeGraph, code);
  // Needed for generateReconstructClassBody's per-member
  // TypeHandler<Ctx, T>::type::describe use, including for a
  // container-typed member - must come after genDefs (see this
  // function's own doc comment for why).
  emitReconstructTypeHandlerSupport(typeGraph, code);
  code += "using __ROOT_TYPE__ = " + cls.name() + ";\n";
  code += "} // namespace\n} // namespace OIInternal\n";
}

// The class/struct slice of the reconstruction scaffold: raw byte replay
// for a flat struct of scalar members, no pointer fixup or nested
// class/container members yet (see
// docs/object-capture-initial-thoughts.md). Mirrors genClassStaticType's
// wire shape - a right-nested chain of types::st::Pair<leaf, ...> - but
// built from the runtime types::dy:: descriptors instead, since this code
// has to decode a shape it didn't just statically encode. Assumes
// OIInternal::__ROOT_TYPE__ already exists in `code` - either from this
// same call's own generateReconstructClassPreamble (standalone reconstruct)
// or from generate()'s equivalent redeclaration for the same root (the
// combined case).
void CodeGen::generateReconstructClassBody(Class& cls,
                                           const std::string& typeToHash,
                                           std::string& code) {
  std::vector<ReconstructableMember> members = collectReconstructableMembers(cls);
  size_t n = members.size();

  // Ctx/DB/TypeHandler need to be in scope here, at namespace scope,
  // because the leaf_i shape declarations just below (also namespace
  // scope, same reason the pre-existing scalar-only version needed
  // `static constexpr`: dy::Pair's fields are references, which need
  // something with a stable address to refer to) reference them. Once
  // declared here they're equally visible inside the function body below,
  // for emitReconstructValue's own use of Ctx/DB/TypeHandler - one
  // declaration serves both.
  code += "using DB = int;\n";
  code += "struct OIReconstructFakeCtx { using DataBuffer = DB; };\n";
  code += "using Ctx = OIReconstructFakeCtx;\n";
  code += "using OIInternal::TypeHandler;\n";
  code += "using OIInternal::oi_capture_bytes;\n";

  // Each member's own leaf shape - TypeHandler<Ctx, T>::type::describe
  // rather than a hand-built dy::Bytes{sizeof(T)}, so a container-typed
  // member (not just a scalar one) gets its own real wire shape (e.g. a
  // vector<string> member's shape is that container's own full,
  // multi-processor Pair chain, not a single fixed-size blob). For a
  // scalar T this still ends up being dy::Bytes{sizeof(T)} - the same
  // value as before, just obtained via the same general mechanism as
  // everything else rather than a special case.
  for (size_t i = 0; i < n; i++) {
    code += "static constexpr auto leaf_" + std::to_string(i) +
            " = TypeHandler<Ctx, " + resolveTypeName(*members[i].type) +
            ">::type::describe;\n";
  }
  if (n > 1) {
    for (size_t i = n - 1; i-- > 0;) {
      std::string rhs = (i == n - 2) ? "leaf_" + std::to_string(n - 1)
                                     : "pair_" + std::to_string(i + 1);
      code += "static constexpr oi::types::dy::Pair pair_" + std::to_string(i) +
              "{leaf_" + std::to_string(i) + ", " + rhs + "};\n";
    }
  }
  const std::string shapeVar = (n == 1) ? "leaf_0" : "pair_0";

  code += "extern \"C\" OIInternal::__ROOT_TYPE__ " + typeToHash +
          "(std::span<const uint8_t> bytes) {\n";
  code += "  std::vector<uint8_t> vec(bytes.begin(), bytes.end());\n";
  code += "  auto it = vec.cbegin();\n";
  code += "  auto parsed = oi::exporters::ParsedData::parse(it, " + shapeVar +
          ");\n";

  size_t idCounter = 0;
  std::vector<std::string> fieldExprs(n);

  if (n == 1) {
    fieldExprs[0] =
        emitReconstructValue(*members[0].type, "parsed", idCounter, code);
  } else {
    // ParsedData::Pair holds Lazy fields, which hold a reference member -
    // that deletes Pair's copy *assignment* (though not construction), so
    // each nesting level gets its own freshly-initialized, uniquely-named
    // variable below rather than reusing/reassigning one. first() must be
    // called before second() at each level - both share the same
    // underlying iterator, so second() would parse from the wrong offset
    // if evaluated first.
    code += "  auto pair_0 = std::get<oi::exporters::ParsedData::Pair>(parsed."
            "val);\n";
    for (size_t i = 0; i < n - 1; i++) {
      fieldExprs[i] = emitReconstructValue(
          *members[i].type, "pair_" + std::to_string(i) + ".first()",
          idCounter, code);
      if (i == n - 2) {
        fieldExprs[i + 1] = emitReconstructValue(
            *members[i + 1].type, "pair_" + std::to_string(i) + ".second()",
            idCounter, code);
      } else {
        code += "  auto next_" + std::to_string(i) + " = pair_" +
                std::to_string(i) + ".second();\n";
        code += "  auto pair_" + std::to_string(i + 1) +
                " = std::get<oi::exporters::ParsedData::Pair>(next_" +
                std::to_string(i) + ".val);\n";
      }
    }
  }

  code += "  return OIInternal::__ROOT_TYPE__{\n";
  for (size_t i = 0; i < n; i++) {
    code += "    ." + std::string(members[i].name) + " = " + fieldExprs[i] +
            ",\n";
  }
  code += "  };\n";
  code += "}\n";
}

// The container slice of the reconstruction scaffold: a "list"-kind
// container (sequence or set) or a "bytes"-kind one (e.g. a string) - not
// yet a map, which needs a third, key+value calling convention - see
// ContainerInfo.h's `reconstruct`/`reconstruct_kind` field docs. Element
// type must be a scalar. Assumes TypeHandler<Ctx, T0> (from
// FuncGen::DefineBasicTypeHandlers) and DEFINE_DESCRIBE-enabled
// oi/types/st.h are already available - either generate() already emitted
// them (the combined case) or the caller emits them itself first (the
// standalone case, see generateReconstruct()).
//
// Unlike a Class root, a container never needs an OIInternal redeclaration:
// std::vector<int32_t> is already a complete, nameable type given its own
// header, with no user-defined-type-visibility problem to work around.
// The recursive core of container reconstruction: decodes one value of
// type `elemType` from the ParsedData produced by evaluating
// `parsedDataExpr` - a C++ expression - *exactly once* (materialized
// immediately into a local, since `parsedDataExpr` may itself be a Lazy
// invocation like `list.values()`, which must never be evaluated twice -
// see drainParsedData's doc comment for why Lazy is this fussy). Emits
// whatever statements decoding `elemType` needs into `code`, and returns
// a C++ expression - always safe to reference multiple times - evaluating
// to a live value of `elemType`'s own C++ type.
//
// `idCounter` is threaded through (incremented once per call, including
// recursive ones) purely to keep every call's emitted local variable
// names distinct - not because two calls can currently land in the same
// C++ scope (they can't yet: a "list"-kind container's element decode
// always happens inside nextElement()'s own lambda body, a fresh scope
// per call site), but because relying on that staying true forever, once
// e.g. a class with multiple container members exists, would be a latent
// bug waiting to happen. Cheap insurance now, not speculative generality.
//
// A Container element recurses: its own `codegen.reconstruct` body is
// spliced in as an immediately-invoked lambda (`[&]() -> Type { ... }()`),
// letting its result be used as an ordinary expression by whichever level
// called it - the same mechanism whether this is the outermost call (see
// generateReconstructContainerBody below) or a nested one.
std::string CodeGen::emitReconstructValue(Type& elemType,
                                          const std::string& parsedDataExpr,
                                          size_t& idCounter,
                                          std::string& code) {
  Type* resolved = &unwrapTypedefs(elemType);

  const std::string v = "v" + std::to_string(idCounter++);
  code += "  auto " + v + "_data = " + parsedDataExpr + ";\n";

  if (auto* prim = dynamic_cast<Primitive*>(resolved)) {
    // NOTE, a known, verified-but-not-guaranteed limitation: see
    // resolveTypeName's comment - Primitive::Kind conflates char with
    // int8_t/signed char, which matters here too if this scalar is ever
    // named as a type (it isn't, in this branch - reconstructScalar<T>
    // only needs T's size/bit-pattern, which int8_t and char share).
    return "oi::exporters::reconstructScalar<" + prim->name() +
           ">(std::get<oi::exporters::ParsedData::Bytes>(" + v + "_data.val))";
  }

  if (dynamic_cast<Enum*>(resolved)) {
    // An enum is trivially copyable and fixed-size, exactly like a
    // Primitive - the write side's generic TypeHandler primary template
    // (FuncGen::DefineBasicTypeHandlers) already captures it the same
    // way (raw bytes via bit_cast), with no enum-specific logic anywhere
    // in the capture path either. reconstructScalar<T>'s own bit_cast
    // works identically on an enum type, so this is the same expression
    // as the Primitive case above, just naming the enum instead - via
    // resolveTypeName, since (unlike a Primitive's builtin name) an
    // enum's declaration lives inside namespace OIInternal and needs
    // that qualification to resolve from this function's global scope.
    return "oi::exporters::reconstructScalar<" + resolveTypeName(*resolved) +
           ">(std::get<oi::exporters::ParsedData::Bytes>(" + v + "_data.val))";
  }

  auto* cont = dynamic_cast<Container*>(resolved);
  if (!cont) {
    throw std::runtime_error(
        "CodeGen::emitReconstructValue: " + resolved->name() +
        " is neither a scalar, an enum, nor a reconstructable container - "
        "not yet supported");
  }

  const ContainerInfo& info = cont->containerInfo_;
  if (info.codegen.reconstruct.empty()) {
    throw std::runtime_error(
        "CodeGen::emitReconstructValue: " + info.typeName +
        " has no `codegen.reconstruct` defined - not yet reconstructable");
  }
  if (info.codegen.reconstructKind != "list" &&
      info.codegen.reconstructKind != "bytes" &&
      info.codegen.reconstructKind != "map") {
    throw std::runtime_error(
        "CodeGen::emitReconstructValue: " + info.typeName +
        " has `codegen.reconstruct` but an unrecognized or missing "
        "`codegen.reconstruct_kind` ('" + info.codegen.reconstructKind +
        "') - expected \"list\", \"bytes\", or \"map\"");
  }
  if (cont->templateParams.empty()) {
    throw std::runtime_error(
        "CodeGen::emitReconstructValue: " + info.typeName +
        " has no template parameters to reconstruct an element type from");
  }
  if (info.codegen.reconstructKind == "map" &&
      cont->templateParams.size() < 2) {
    throw std::runtime_error(
        "CodeGen::emitReconstructValue: " + info.typeName +
        " is \"map\"-kind but has fewer than 2 template parameters to "
        "reconstruct a key and a value type from");
  }

  const auto& processors = info.codegen.processors;
  size_t n = processors.size();
  if (n == 0) {
    throw std::runtime_error(
        "CodeGen::emitReconstructValue: " + info.typeName +
        " has no codegen.processor entries to decode captured bytes from");
  }

  // Everything below - the processor-chain walk, the kind-specific
  // preamble (length/nextElement/nextEntry/contentBytes), and the
  // reconstruct body itself - is emitted *inside* this result lambda,
  // not before it. Those preamble names are bare (not idCounter-suffixed,
  // since a container's toml `reconstruct` text references them
  // literally - see ContainerInfo.h's calling-convention doc), so two
  // sibling container members reconstructed into the same enclosing
  // function (e.g. a class with both a vector member and a map member)
  // would otherwise redeclare `length` at the same shared scope and fail
  // to compile. Scoping them inside this lambda, which already exists
  // per container instance, fixes that for free.
  const std::string resultVar = v + "_result";
  code += "  auto " + resultVar + " = [&]() -> " + resolveTypeName(*cont) +
          " {\n";

  // Walk this container's own processor chain - exactly the same
  // "discard everything but the last, but drain every discarded one
  // fully" logic as the top level (see generateReconstructContainerBody's
  // original comment, preserved in spirit here). v_data is already
  // shaped per this container's own TypeHandler<Ctx,T>::type - either
  // from an explicit ParsedData::parse call (the outermost call) or from
  // a Lazy invocation like list.values() that used that same describe
  // value internally (a nested call) - either way no second parse is
  // needed here, just walking what's already there.
  std::string lastVal = v + "_data";
  if (n > 1) {
    code += "  auto " + v + "_pair_0 = std::get<oi::exporters::ParsedData::"
            "Pair>(" + lastVal + ".val);\n";
    code += "  oi::exporters::drainParsedData(" + v + "_pair_0.first());\n";
    for (size_t i = 0; i < n - 1; i++) {
      if (i == n - 2) {
        lastVal = v + "_last";
        code += "  auto " + lastVal + " = " + v + "_pair_" +
                std::to_string(i) + ".second();\n";
      } else {
        code += "  auto " + v + "_next_" + std::to_string(i) + " = " + v +
                "_pair_" + std::to_string(i) + ".second();\n";
        code += "  auto " + v + "_pair_" + std::to_string(i + 1) +
                " = std::get<oi::exporters::ParsedData::Pair>(" + v +
                "_next_" + std::to_string(i) + ".val);\n";
        code += "  oi::exporters::drainParsedData(" + v + "_pair_" +
                std::to_string(i + 1) + ".first());\n";
      }
    }
  }

  if (info.codegen.reconstructKind == "list") {
    code += "  auto " + v + "_list = std::get<oi::exporters::ParsedData::"
            "List>(" + lastVal + ".val);\n";
    code += "  size_t length = " + v + "_list.length;\n";

    // Recurse for the element type - its own decode statements land
    // inside nextElement()'s lambda body, its own fresh C++ scope, so
    // reusing unprefixed names like `length`/`nextElement` again one
    // level down (for a container-of-containers) can't collide with
    // this level's.
    std::string nextElemCode;
    std::string nextElemExpr =
        emitReconstructValue(cont->templateParams[0].type(),
                             v + "_list.values()", idCounter, nextElemCode);
    code += "  auto nextElement = [&]() {\n";
    code += nextElemCode;
    code += "    return " + nextElemExpr + ";\n";
    code += "  };\n";
  } else if (info.codegen.reconstructKind == "map") {
    // A map's content processor is a List of (key, value) Pairs (see
    // std_map_type.toml) - one level of structure beyond "list"-kind's
    // bare element list, so nextEntry() has to peel off that Pair itself
    // (not something emitReconstructValue's generic Type-based recursion
    // handles - a wire-level Pair isn't a reconstructable C++ type on its
    // own) before recursing for the key and the value individually.
    code += "  auto " + v + "_list = std::get<oi::exporters::ParsedData::"
            "List>(" + lastVal + ".val);\n";
    code += "  size_t length = " + v + "_list.length;\n";

    std::string keyCode, valueCode;
    std::string keyExpr = emitReconstructValue(
        cont->templateParams[0].type(), v + "_entry.first()", idCounter,
        keyCode);
    std::string valueExpr = emitReconstructValue(
        cont->templateParams[1].type(), v + "_entry.second()", idCounter,
        valueCode);
    code += "  auto nextEntry = [&]() {\n";
    code += "    auto " + v +
            "_entry = std::get<oi::exporters::ParsedData::Pair>(" + v +
            "_list.values().val);\n";
    code += keyCode;
    code += valueCode;
    code += "    return std::make_pair(" + keyExpr + ", " + valueExpr +
            ");\n";
    code += "  };\n";
  } else {
    // "bytes": the whole reconstructable content is one contiguous
    // captured byte blob (e.g. a string's characters), not a per-element
    // list - nothing left to decode, just hand the raw bytes over.
    code += "  auto contentBytes = std::get<oi::exporters::ParsedData::"
            "DynBytes>(" + lastVal + ".val).value;\n";
  }

  // T0 (and T1, for a "map"-kind container) must mean *this* container's
  // own template parameters inside its own reconstruct body - shadowing
  // whatever an enclosing level (if any) already declared. Without this,
  // a nested container's reconstruct body (e.g. a vector<string>'s
  // element string) would incorrectly see the outermost container's T0
  // instead of its own, since bare `T0`/`T1` are otherwise just
  // unqualified names looked up in the enclosing scope.
  code += "    using T0 = " +
          resolveTypeName(cont->templateParams[0].type()) + ";\n";
  if (cont->templateParams.size() >= 2) {
    code += "    using T1 = " +
            resolveTypeName(cont->templateParams[1].type()) + ";\n";
  }
  code += (boost::format(info.codegen.reconstruct) % info.typeName).str();
  code += "\n  }();\n";

  return resultVar;
}

void CodeGen::generateReconstructContainerBody(Container& container,
                                               const std::string& typeToHash,
                                               std::string& code) {
  if (container.templateParams.empty()) {
    throw std::runtime_error(
        "CodeGen::generateReconstructContainerBody: " +
        container.containerInfo_.typeName +
        " has no template parameters to reconstruct an element type from");
  }

  const std::string containerType = resolveTypeName(container);
  const std::string t0Name = resolveTypeName(container.templateParams[0].type());

  // The container's full wire shape, exactly as genContainerTypeHandler
  // builds it for the write side (see CodeGen.cpp above) - a right-nested
  // Pair of every processor's type, in order. Reused verbatim (not
  // re-derived) specifically so this can never drift out of sync with what
  // the write side actually produced. This is the one and only explicit
  // ParsedData::parse call needed anywhere in this reconstruction - every
  // nested element beneath it (see emitReconstructValue) is already
  // correctly shaped by its own enclosing List/Pair's own Lazy machinery.
  const auto& processors = container.containerInfo_.codegen.processors;
  size_t n = processors.size();
  if (n == 0) {
    throw std::runtime_error(
        "CodeGen::generateReconstructContainerBody: " +
        container.containerInfo_.typeName +
        " has no codegen.processor entries to decode captured bytes from");
  }
  std::string shapeType;
  for (size_t i = 0; i < n; i++) {
    if (i != n - 1)
      shapeType += "types::st::Pair<DB, ";
    shapeType += processors[i].type;
    if (i != n - 1)
      shapeType += ", ";
  }
  shapeType += std::string(n - 1, '>');

  code += "extern \"C\" " + containerType + " " + typeToHash +
          "(std::span<const uint8_t> bytes) {\n";
  code += "  using DB = int;\n";
  code += "  struct OIReconstructFakeCtx { using DataBuffer = DB; };\n";
  code += "  using Ctx = OIReconstructFakeCtx;\n";
  // T0 (and T1, for a map) is the outermost container's own template
  // parameter(s) - needed as bare names for its processor type strings
  // (e.g. seq_type.toml's `typename TypeHandler<Ctx, T0>::type`, or
  // std_map_type.toml's ...<Ctx, T0>/...<Ctx, T1>) to resolve; a nested
  // element's own T0/T1 (if it's itself a container) is handled the same
  // way, but as a local inside emitReconstructValue's own lambda scope,
  // not here.
  code += "  using T0 = " + t0Name + ";\n";
  if (container.templateParams.size() >= 2) {
    code += "  using T1 = " +
            resolveTypeName(container.templateParams[1].type()) + ";\n";
  }
  // TypeHandler and oi_capture_bytes are always emitted inside
  // namespace OIInternal { namespace {...} } - both by generate() (the
  // combined case) and by generateReconstruct() itself (the standalone
  // case, which wraps its own FuncGen::DefineBasicTypeHandlers call the
  // same way) specifically so these lines resolve identically either way.
  code += "  using OIInternal::TypeHandler;\n";
  code += "  using OIInternal::oi_capture_bytes;\n";
  // A map-shaped container's processor type is
  // std::conditional_t<captureKeys, <uses CaptureKeyHandler>, <doesn't>>
  // (see std_map_type.toml) - std::conditional_t requires *both* branches
  // to name-resolve regardless of which one captureKeys actually selects
  // (unlike `if constexpr`, it doesn't discard the unused branch from
  // lookup), so CaptureKeyHandler has to be reachable here even though
  // captureKeys is always false in practice for reconstruction.
  code += "  using OIInternal::CaptureKeyHandler;\n";
  // A map-shaped container's content processor is conditioned on
  // captureKeys (see std_map_type.toml) - a member of that container's
  // own TypeHandler specialization normally, invisible here since
  // shapeType (below) reuses the processor text outside that class body.
  // Reconstruction never enables key capture, so this is always false in
  // practice, but reads the real field rather than hard-coding that.
  code += "  constexpr bool captureKeys = " +
          std::string(container.containerInfo_.captureKeys ? "true"
                                                            : "false") +
          ";\n";
  code += "  std::vector<uint8_t> vec(bytes.begin(), bytes.end());\n";
  code += "  auto it = vec.cbegin();\n";

  size_t idCounter = 0;
  std::string valueExpr = emitReconstructValue(
      container,
      "oi::exporters::ParsedData::parse(it, " + shapeType + "::describe)",
      idCounter, code);

  code += "  return " + valueExpr + ";\n";
  code += "}\n";
}

void CodeGen::generateReconstruct(TypeGraph& typeGraph,
                                  std::string& code,
                                  RootFunctionName rootName) {
  code.clear();

  assert(typeGraph.rootTypes().size() == 1);
  Type& rootType = typeGraph.rootTypes()[0];

  auto* primitive = dynamic_cast<Primitive*>(&rootType);
  auto* cls = dynamic_cast<Class*>(&rootType);
  auto* container = dynamic_cast<Container*>(&rootType);
  if (!primitive && !cls && !container) {
    throw std::runtime_error(
        "CodeGen::generateReconstruct: only scalar, flat-struct, and "
        "scalar-element-container root types are currently supported (see "
        "docs/object-capture-initial-thoughts.md - pointer fixup is not "
        "yet implemented)");
  }

  // Same preamble generate() emits (see OITraceCode.cpp): among other
  // things, it works around newer glibc's __malloc__ attribute syntax that
  // this clang version doesn't parse, which the synthetic ParsedData.h/dy.h
  // headers below would otherwise hit via their own <cstdlib> include.
  addPreprocessorDefines(config_, code);
  code += headers::oi_OITraceCode_cpp;

  code += "#include <oi/exporters/ParsedData.h>\n";
  code += "#include <oi/types/dy.h>\n";
  code += "#include <cstdint>\n";
  code += "#include <span>\n";
  code += "#include <vector>\n\n";

  if (container || cls) {
    // Both a container root and a class root can now need the
    // TypeHandler<Ctx, T>/st:: describe machinery - a class might have a
    // container-typed member (see generateReconstructClassBody). generate()'s
    // equivalent (combined) path already has all of this via
    // addIncludes()/DefineBasicTypeHandlers()/etc, so this is confined to
    // the standalone case.
    //
    // Reuses addIncludes() rather than hand-listing headers specifically
    // so a *nested* container's header (e.g. <string>, for a
    // vector<string> root or member) is picked up too - addIncludes
    // already walks every reachable type in typeGraph.finalTypes for
    // exactly this purpose on generate()'s side.
    addIncludes(typeGraph, config_, code);
    // DefineBasicTypeHandlers' own emitted code uses inst::/result::/
    // ParsedData/types::st:: unqualified, and (in its pointer branch, which
    // a scalar T never instantiates but still has to parse) the
    // JLOG/JLOGPTR macros - all normally brought into scope by generate()'s
    // preamble, which this standalone path doesn't otherwise run.
    // Deliberately no `using namespace oi::detail;` here (unlike
    // generate()'s equivalent preamble): that only compiles there because
    // generate() has already declared real content under
    // oi::detail::DataBuffer earlier in the same file, which is what makes
    // oi::detail a name the compiler recognizes at all - nothing in this
    // path ever opens that namespace, and nothing this path emits needs it.
    code += "using namespace oi;\n";
    code += "using oi::exporters::ParsedData;\n";
    code += "using namespace oi::exporters;\n";
    FuncGen::DefineJitLog(code, config_.features);
  }

  if (container) {
    // TypeHandler is wrapped in the same namespace OIInternal { namespace
    // { ... } } generate() itself uses, so generateReconstructContainerBody's
    // `using OIInternal::TypeHandler;` resolves identically regardless of
    // which path produced it. A Class root's equivalent block lives inside
    // generateReconstructClassPreamble instead (called below), since there
    // it has to run after the struct's own redeclaration, not before -
    // see that function's doc comment for why.
    code += "namespace OIInternal {\nnamespace {\n";
    // addStandardTypeHandlers (inside emitReconstructTypeHandlerSupport)
    // unconditionally sets up a TypeHandler<Ctx, OIArray<T0,N0>>
    // specialization (for padding members generate() itself would have
    // added) - OIArray<> the template needs to actually exist for that to
    // compile, even though no reconstructable root here ever has padding
    // members of its own. (The class case gets this from
    // generateReconstructClassPreamble's own defineInternalTypes call
    // instead, needed there for a different reason - see its comment.)
    defineInternalTypes(code);
    emitReconstructTypeHandlerSupport(typeGraph, code);
    code += "} // namespace\n} // namespace OIInternal\n";
  }

  const auto& typeToHash = std::visit(
      [](const auto& v) -> const std::string& {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<ExactName, T> ||
                      std::is_same_v<HashedComponent, T>) {
          return v.name;
        } else {
          static_assert(always_false_v<T>, "missing visit");
        }
      },
      rootName);

  if (primitive) {
    generateReconstructScalar(*primitive, typeToHash, code);
  } else if (cls) {
    generateReconstructClassPreamble(typeGraph, *cls, code);
    generateReconstructClassBody(*cls, typeToHash, code);
  } else {
    generateReconstructContainerBody(*container, typeToHash, code);
  }

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "Generated reconstruct code:\n";
    std::cerr << code;
  }
}

// The combined introspect+reconstruct case (same root T, one oilgen
// invocation - see docs/object-capture-initial-thoughts.md and
// OIGenerator::generate()'s same-type check): appends reconstructImpl<T>'s
// function body to `code` that generate() already populated for
// introspectImpl<T>, reusing generate()'s OIInternal::__ROOT_TYPE__
// redeclaration instead of emitting a second, colliding copy of it. Unlike
// generateReconstruct(), this never clears `code` and never emits the
// includes/glibc-compat preamble - generate() already did both, and this
// is only ever called immediately after it for the same TypeGraph.
void CodeGen::appendReconstructFunctionBody(TypeGraph& typeGraph,
                                            std::string& code,
                                            RootFunctionName rootName) {
  assert(typeGraph.rootTypes().size() == 1);
  Type& rootType = typeGraph.rootTypes()[0];

  auto* primitive = dynamic_cast<Primitive*>(&rootType);
  auto* cls = dynamic_cast<Class*>(&rootType);
  auto* container = dynamic_cast<Container*>(&rootType);
  if (!primitive && !cls && !container) {
    throw std::runtime_error(
        "CodeGen::appendReconstructFunctionBody: only scalar, flat-struct, "
        "and scalar-element-container root types are currently supported "
        "(see docs/object-capture-initial-thoughts.md - pointer fixup is "
        "not yet implemented)");
  }

  const auto& typeToHash = std::visit(
      [](const auto& v) -> const std::string& {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<ExactName, T> ||
                      std::is_same_v<HashedComponent, T>) {
          return v.name;
        } else {
          static_assert(always_false_v<T>, "missing visit");
        }
      },
      rootName);

  if (primitive) {
    generateReconstructScalar(*primitive, typeToHash, code);
  } else if (cls) {
    generateReconstructClassBody(*cls, typeToHash, code);
  } else {
    generateReconstructContainerBody(*container, typeToHash, code);
  }

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "Generated (appended) reconstruct code:\n";
    std::cerr << code;
  }
}

}  // namespace oi::detail
