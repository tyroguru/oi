// test/oil/oil_vector_leaf_structs.cpp

#include <oi/oi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#ifndef OIL_TEST_CONFIG_PATH
#error "OIL_TEST_CONFIG_PATH must be defined"
#endif

namespace {

struct Leaf {
  std::uint32_t value;
  bool enabled;
};

struct Root {
  std::uint64_t id;
  std::vector<Leaf> leaves;
};

void printStringViewSpan(std::span<const std::string_view> values) {
  std::cout << '[';

  bool first = true;
  for (const auto value : values) {
    if (!first) {
      std::cout << ", ";
    }

    std::cout << value;
    first = false;
  }

  std::cout << ']';
}

bool typePathContains(const oi::result::Element& element,
                      std::string_view name) {
  return std::find(element.type_path.begin(), element.type_path.end(), name) !=
         element.type_path.end();
}

void printElement(const oi::result::Element& element) {
  std::cout << "name=\"" << element.name << "\""
            << " static_size=" << element.static_size
            << " exclusive_size=" << element.exclusive_size
            << " primitive=" << std::boolalpha << element.is_primitive;

  if (element.container_stats.has_value()) {
    std::cout << " length=" << element.container_stats->length
              << " capacity=" << element.container_stats->capacity;
  }

  if (!element.va_intervals.empty()) {
    std::cout << " num_intervals: " << element.va_intervals.size();
  }

  std::cout << " type_path=";
  printStringViewSpan(element.type_path);

  std::cout << " type_names=";
  printStringViewSpan(element.type_names);

  std::cout << " va_intervals=[";

  bool firstInterval = true;
  for (const auto& interval : element.va_intervals) {
    if (!firstInterval) {
      std::cout << ", ";
    }

    const auto end = interval.base + interval.size;

    std::cout << '[' << std::showbase << std::hex << interval.base << ", "
              << end << std::dec << std::noshowbase << ")"
              << " size=" << interval.size;

    firstInterval = false;
  }

  std::cout << ']';

  std::cout << '\n';
}

void printLeavesStorage(const Root& object) {
  const auto& leaves = object.leaves;

  std::cout << "Root object address:          "
            << static_cast<const void*>(&object) << '\n';

  std::cout << "Root::leaves vector address:  "
            << static_cast<const void*>(&leaves) << '\n';

  std::cout << "Root::leaves.data() address:  "
            << static_cast<const void*>(leaves.data()) << '\n';

  std::cout << "Root::leaves size:            " << leaves.size() << '\n'
            << "Root::leaves capacity:        " << leaves.capacity() << '\n'
            << "sizeof(Leaf):                 " << sizeof(Leaf) << '\n';

  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const Leaf& leaf = leaves[i];

    std::cout << "leaves[" << i << "]"
              << " object_address=" << static_cast<const void*>(&leaf)
              << " value_address=" << static_cast<const void*>(&leaf.value)
              << " enabled_address=" << static_cast<const void*>(&leaf.enabled)
              << " value=" << leaf.value << " enabled=" << std::boolalpha
              << leaf.enabled << '\n';
  }
}

}  // namespace

int main() {
  try {
    Root object{
        .id = 123,
        .leaves =
            {
                Leaf{.value = 10, .enabled = true},
                Leaf{.value = 20, .enabled = false},
                Leaf{.value = 30, .enabled = true},
            },
    };

    object.leaves.reserve(1000);

    printLeavesStorage(object);

    oi::GeneratorOptions opts;
    opts.debugLevel = 0;
    opts.configFilePaths.emplace_back(OIL_TEST_CONFIG_PATH);

    const auto result = oi::setupAndIntrospect(object, opts);

    if (!result.has_value()) {
      std::cerr << "OIL introspection did not complete. "
                << "Another thread may be initialising this type, or JIT setup "
                   "failed.\n";
      return EXIT_FAILURE;
    }

    std::size_t elementCount = 0;
    std::size_t leafObjectCount = 0;
    std::size_t leafValueCount = 0;
    std::size_t leafEnabledCount = 0;

    bool sawId = false;
    bool sawLeaves = false;
    bool sawLeavesContainerStats = false;
    bool sawCorrectLeavesLength = false;
    bool sawPlausibleLeavesCapacity = false;

    for (const auto& element : *result) {
      ++elementCount;
      printElement(element);

      if (element.name == "id") {
        sawId = true;
      }

      if (element.name == "leaves") {
        sawLeaves = true;

        if (element.container_stats.has_value()) {
          sawLeavesContainerStats = true;
          sawCorrectLeavesLength =
              element.container_stats->length == object.leaves.size();
          sawPlausibleLeavesCapacity =
              element.container_stats->capacity >= object.leaves.size();
        }
      }

      const bool underLeaves = typePathContains(element, "leaves");

      if (underLeaves && element.static_size == sizeof(Leaf) &&
          !element.is_primitive) {
        ++leafObjectCount;
      }

      if (underLeaves && element.name == "value") {
        ++leafValueCount;
      }

      if (underLeaves && element.name == "enabled") {
        ++leafEnabledCount;
      }
    }

    std::cout << "element_count=" << elementCount << '\n'
              << "leaf_object_count=" << leafObjectCount << '\n'
              << "leaf_value_count=" << leafValueCount << '\n'
              << "leaf_enabled_count=" << leafEnabledCount << '\n';

    bool ok = true;

    if (!sawId) {
      std::cerr << "Expected to see Root::id\n";
      ok = false;
    }

    if (!sawLeaves) {
      std::cerr << "Expected to see Root::leaves\n";
      ok = false;
    }

    if (!sawLeavesContainerStats) {
      std::cerr << "Expected vector field to have container stats\n";
      ok = false;
    }

    if (!sawCorrectLeavesLength) {
      std::cerr << "Expected vector length to be " << object.leaves.size()
                << '\n';
      ok = false;
    }

    if (!sawPlausibleLeavesCapacity) {
      std::cerr << "Expected vector capacity to be at least "
                << object.leaves.size() << '\n';
      ok = false;
    }

    if (leafObjectCount != object.leaves.size()) {
      std::cerr << "Expected to see " << object.leaves.size()
                << " Leaf object elements, saw " << leafObjectCount << '\n';
      ok = false;
    }

    if (leafValueCount != object.leaves.size()) {
      std::cerr << "Expected to see " << object.leaves.size()
                << " Leaf::value fields, saw " << leafValueCount << '\n';
      ok = false;
    }

    if (leafEnabledCount != object.leaves.size()) {
      std::cerr << "Expected to see " << object.leaves.size()
                << " Leaf::enabled fields, saw " << leafEnabledCount << '\n';
      ok = false;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
