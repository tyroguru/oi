#include <oi/IntrospectionResult.h>
#include <oi/exporters/ParsedData.h>
#include <oi/exporters/inst.h>
#include <oi/oi.h>

#include <cstdint>
#include <iostream>
#include <type_traits>
#include <vector>

int main() {
  // Basic compile-time checks that public OIL types are visible.
  static_assert(std::is_class_v<oi::IntrospectionResult>);
  static_assert(std::is_class_v<oi::exporters::ParsedData>);

  // Check that public enum/API symbols are usable.
  [[maybe_unused]] auto feature = oi::Feature::ChaseRawPointers;

  // Construct the smallest plausible instruction tree.
  //
  // This does not attempt real object introspection. It only verifies that an
  // external program can include OIL headers, link against liboil, and use
  // exported result/instruction types.
  static constexpr std::array<std::string_view, 1> typeNames = {
      "SmokeType",
  };

  static constexpr std::array<oi::exporters::inst::Field, 0> fields = {};
  static constexpr std::array<oi::exporters::inst::ProcessorInst, 0>
      processors = {};

  static constexpr oi::exporters::inst::Field root{
      sizeof(std::uint64_t),
      "root",
      typeNames,
      fields,
      processors,
      true,
  };

  std::vector<std::uint8_t> buffer;
  oi::IntrospectionResult result{
      std::move(buffer),
      std::cref(root),
  };

  auto begin = result.begin();
  auto end = result.end();

  // We do not care whether this result contains useful data. The point is to
  // instantiate and link the public liboil machinery.
  [[maybe_unused]] bool empty_or_valid = begin == end || begin != end;

  std::cout << "liboil smoke test passed\n";
  return 0;
}
