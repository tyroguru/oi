// test/oil/oil_set_leaf_structs.cpp

#include <oi/oi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <ostream>
#include <set>
#include <string_view>

#ifndef OIL_TEST_CONFIG_PATH
#error "OIL_TEST_CONFIG_PATH must be defined"
#endif

namespace {

struct Leaf {
  std::uint32_t value;
  bool enabled;
};

bool operator<(const Leaf& lhs, const Leaf& rhs) {
  return lhs.value < rhs.value;
}

struct Root {
  std::set<Leaf> leaves;
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

void printSetIntervals(const oi::result::Element& element) {
  std::cout << "std::set captured intervals: " << element.va_intervals.size()
            << '\n';

  for (std::size_t i = 0; i < element.va_intervals.size(); ++i) {
    std::cout << "  interval[" << i << "]=";
    printInterval(std::cout, element.va_intervals[i]);
    std::cout << '\n';
  }
}

void printExpectedAddresses(const Root& object) {
  std::cout << "std::set object address: "
            << static_cast<const void*>(&object.leaves)
            << " size=" << sizeof(object.leaves) << '\n';

#ifdef __GLIBCXX__
  std::size_t node_index = 0;
  for (auto it = object.leaves.begin(); it != object.leaves.end(); ++it) {
    const auto* node = static_cast<const std::_Rb_tree_node<Leaf>*>(it._M_node);

    std::cout << "  node[" << node_index
              << "] address=" << static_cast<const void*>(node)
              << " size=" << sizeof(*node) << '\n';
    ++node_index;
  }
#endif

  std::size_t leaf_index = 0;
  for (const Leaf& leaf : object.leaves) {
    std::cout << "  leaf[" << leaf_index
              << "] address=" << static_cast<const void*>(&leaf)
              << " size=" << sizeof(leaf) << " value=" << leaf.value
              << " enabled=" << std::boolalpha << leaf.enabled << '\n';
    ++leaf_index;
  }
}

}  // namespace

int main() {
  try {
    const Root object{
        .leaves =
            {
                Leaf{.value = 10, .enabled = true},
                Leaf{.value = 20, .enabled = false},
                Leaf{.value = 30, .enabled = true},
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

    bool sawSetElement = false;
    std::size_t leafValueFields = 0;

    for (const auto& element : *result) {
      if (element.name == "leaves") {
        sawSetElement = true;

        printExpectedAddresses(object);
        printSetIntervals(element);

        const auto& stats = element.container_stats;
        if (!stats.has_value() || stats->length != object.leaves.size() ||
            stats->capacity != object.leaves.size()) {
          std::cerr << "Unexpected std::set container stats\n";
          return EXIT_FAILURE;
        }

#ifdef __GLIBCXX__
        if (element.va_intervals.size() != object.leaves.size() + 1) {
          std::cerr << "Expected set object plus one VA interval per node\n";
          return EXIT_FAILURE;
        }

        if (!hasExactInterval(element,
                              reinterpret_cast<uintptr_t>(&object.leaves),
                              sizeof(object.leaves))) {
          std::cerr << "Expected VA interval for std::set object\n";
          return EXIT_FAILURE;
        }

        for (auto it = object.leaves.begin(); it != object.leaves.end(); ++it) {
          const auto* node =
              static_cast<const std::_Rb_tree_node<Leaf>*>(it._M_node);

          if (!hasExactInterval(
                  element, reinterpret_cast<uintptr_t>(node), sizeof(*node))) {
            std::cerr << "Expected VA interval for each std::set node\n";
            return EXIT_FAILURE;
          }
        }
#else
        if (element.va_intervals.empty()) {
          std::cerr << "Expected std::set VA intervals\n";
          return EXIT_FAILURE;
        }
#endif

        for (const Leaf& leaf : object.leaves) {
          if (!hasSingleCoveringInterval(
                  element, reinterpret_cast<uintptr_t>(&leaf), sizeof(leaf))) {
            std::cerr << "Expected one node VA interval covering each set "
                         "element\n";
            return EXIT_FAILURE;
          }
        }
      }

      if (typePathContains(element, "leaves") && element.name == "value") {
        ++leafValueFields;
      }
    }

    if (!sawSetElement) {
      std::cerr << "Expected to find Root::leaves\n";
      return EXIT_FAILURE;
    }

    if (leafValueFields != object.leaves.size()) {
      std::cerr << "Expected traversal to visit each set element\n";
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
