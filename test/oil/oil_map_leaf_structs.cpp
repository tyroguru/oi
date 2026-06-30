// test/oil/oil_map_leaf_structs.cpp

#include <oi/oi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <ostream>
#include <string_view>

#ifndef OIL_TEST_CONFIG_PATH
#error "OIL_TEST_CONFIG_PATH must be defined"
#endif

namespace {

struct Leaf {
  std::uint32_t value;
  bool enabled;
};

struct Root {
  std::map<std::uint32_t, Leaf> leaves;
};

bool typePathContains(const oi::result::Element& element,
                      std::string_view name) {
  return std::find(element.type_path.begin(), element.type_path.end(), name) !=
         element.type_path.end();
}

bool hasExactInterval(const oi::result::Element& element,
                      uintptr_t base,
                      std::size_t size) {
  return std::any_of(element.va_intervals.begin(),
                     element.va_intervals.end(),
                     [base, size](const auto& interval) {
                       return interval.base == base && interval.size == size;
                     });
}

bool intervalContains(const oi::result::Element::VAInterval& interval,
                      uintptr_t base,
                      std::size_t size) {
  return interval.base <= base && base + size <= interval.base + interval.size;
}

bool hasSingleCoveringInterval(const oi::result::Element& element,
                               uintptr_t base,
                               std::size_t size) {
  return std::count_if(element.va_intervals.begin(),
                       element.va_intervals.end(),
                       [base, size](const auto& interval) {
                         return intervalContains(interval, base, size);
                       }) == 1;
}

void printInterval(std::ostream& out,
                   const oi::result::Element::VAInterval& interval) {
  out << "[" << std::showbase << std::hex << interval.base << ", "
      << interval.base + interval.size << std::dec << std::noshowbase
      << ") size=" << interval.size;
}

void printMapIntervals(const oi::result::Element& element) {
  std::cout << "std::map captured intervals: " << element.va_intervals.size()
            << '\n';

  for (std::size_t i = 0; i < element.va_intervals.size(); ++i) {
    std::cout << "  interval[" << i << "]=";
    printInterval(std::cout, element.va_intervals[i]);
    std::cout << '\n';
  }
}

void printExpectedAddresses(const Root& object) {
  std::cout << "std::map object address: "
            << static_cast<const void*>(&object.leaves)
            << " size=" << sizeof(object.leaves) << '\n';

#ifdef __GLIBCXX__
  std::size_t node_index = 0;
  for (auto it = object.leaves.begin(); it != object.leaves.end(); ++it) {
    using ValueType = std::pair<const std::uint32_t, Leaf>;
    const auto* node =
        static_cast<const std::_Rb_tree_node<ValueType>*>(it._M_node);

    std::cout << "  node[" << node_index
              << "] address=" << static_cast<const void*>(node)
              << " size=" << sizeof(*node) << '\n';
    ++node_index;
  }
#endif

  std::size_t entry_index = 0;
  for (const auto& [key, leaf] : object.leaves) {
    std::cout << "  entry[" << entry_index
              << "] key_address=" << static_cast<const void*>(&key)
              << " leaf_address=" << static_cast<const void*>(&leaf)
              << " leaf_size=" << sizeof(leaf) << " key=" << key
              << " value=" << leaf.value << " enabled=" << std::boolalpha
              << leaf.enabled << '\n';
    ++entry_index;
  }
}

}  // namespace

int main() {
  try {
    const Root object{
        .leaves =
            {
                {1, Leaf{.value = 10, .enabled = true}},
                {2, Leaf{.value = 20, .enabled = false}},
                {3, Leaf{.value = 30, .enabled = true}},
            },
    };

    oi::GeneratorOptions opts;
    opts.debugLevel = 0;
    opts.configFilePaths.emplace_back(OIL_TEST_CONFIG_PATH);

    const auto result = oi::setupAndIntrospect(object, opts);
    if (!result.has_value()) {
      std::cerr << "OIL introspection did not complete\n";
      return EXIT_FAILURE;
    }

    bool sawMapElement = false;
    std::size_t keyFields = 0;
    std::size_t leafEnabledFields = 0;

    for (const auto& element : *result) {
      if (element.name == "leaves") {
        sawMapElement = true;

        printExpectedAddresses(object);
        printMapIntervals(element);

        const auto& stats = element.container_stats;
        if (!stats.has_value() || stats->length != object.leaves.size() ||
            stats->capacity != object.leaves.size()) {
          std::cerr << "Unexpected std::map container stats\n";
          return EXIT_FAILURE;
        }

#ifdef __GLIBCXX__
        if (element.va_intervals.size() != object.leaves.size() + 1) {
          std::cerr << "Expected map object plus one VA interval per node\n";
          return EXIT_FAILURE;
        }

        if (!hasExactInterval(element,
                              reinterpret_cast<uintptr_t>(&object.leaves),
                              sizeof(object.leaves))) {
          std::cerr << "Expected VA interval for std::map object\n";
          return EXIT_FAILURE;
        }

        for (auto it = object.leaves.begin(); it != object.leaves.end(); ++it) {
          using ValueType = std::pair<const std::uint32_t, Leaf>;
          const auto* node =
              static_cast<const std::_Rb_tree_node<ValueType>*>(it._M_node);

          if (!hasExactInterval(
                  element, reinterpret_cast<uintptr_t>(node), sizeof(*node))) {
            std::cerr << "Expected VA interval for each std::map node\n";
            return EXIT_FAILURE;
          }
        }
#else
        if (element.va_intervals.empty()) {
          std::cerr << "Expected std::map VA intervals\n";
          return EXIT_FAILURE;
        }
#endif

        for (const auto& [key, leaf] : object.leaves) {
          if (!hasSingleCoveringInterval(
                  element, reinterpret_cast<uintptr_t>(&key), sizeof(key))) {
            std::cerr << "Expected one node VA interval covering each map key\n";
            return EXIT_FAILURE;
          }

          if (!hasSingleCoveringInterval(
                  element, reinterpret_cast<uintptr_t>(&leaf), sizeof(leaf))) {
            std::cerr << "Expected one node VA interval covering each map value\n";
            return EXIT_FAILURE;
          }
        }
      }

      if (typePathContains(element, "leaves") && element.name == "key") {
        ++keyFields;
      }

      if (typePathContains(element, "leaves") && element.name == "enabled") {
        ++leafEnabledFields;
      }
    }

    if (!sawMapElement) {
      std::cerr << "Expected to find Root::leaves\n";
      return EXIT_FAILURE;
    }

    if (keyFields != object.leaves.size()) {
      std::cerr << "Expected traversal to visit each map key\n";
      return EXIT_FAILURE;
    }

    if (leafEnabledFields != object.leaves.size()) {
      std::cerr << "Expected traversal to visit each map value object\n";
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
