// test/oil/oil_array_leaf_structs.cpp

#include <oi/oi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
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
  std::array<Leaf, 3> leaves;
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

void printInterval(std::ostream& out,
                   const oi::result::Element::VAInterval& interval) {
  out << "[" << std::showbase << std::hex << interval.base << ", "
      << interval.base + interval.size << std::dec << std::noshowbase
      << ") size=" << interval.size;
}

void printArrayIntervals(const oi::result::Element& element) {
  std::cout << "std::array captured intervals: " << element.va_intervals.size()
            << '\n';

  for (std::size_t i = 0; i < element.va_intervals.size(); ++i) {
    std::cout << "  interval[" << i << "]=";
    printInterval(std::cout, element.va_intervals[i]);
    std::cout << '\n';
  }
}

void printExpectedAddresses(const Root& object) {
  std::cout << "std::array object address: "
            << static_cast<const void*>(&object.leaves)
            << " size=" << sizeof(object.leaves) << '\n';

  for (std::size_t i = 0; i < object.leaves.size(); ++i) {
    const Leaf& leaf = object.leaves[i];
    std::cout << "  leaf[" << i
              << "] address=" << static_cast<const void*>(&leaf)
              << " size=" << sizeof(leaf) << '\n';
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

    bool sawArrayElement = false;
    std::size_t leafValueFields = 0;

    for (const auto& element : *result) {
      if (element.name == "leaves") {
        sawArrayElement = true;

        printExpectedAddresses(object);
        printArrayIntervals(element);

        const auto& stats = element.container_stats;
        if (!stats.has_value() || stats->length != object.leaves.size() ||
            stats->capacity != object.leaves.size()) {
          std::cerr << "Unexpected std::array container stats\n";
          return EXIT_FAILURE;
        }

        if (element.va_intervals.size() != 1) {
          std::cerr
              << "Expected one VA interval for inline std::array storage\n";
          return EXIT_FAILURE;
        }

        if (!hasExactInterval(element,
                              reinterpret_cast<uintptr_t>(&object.leaves),
                              sizeof(object.leaves))) {
          std::cerr << "Expected VA interval for std::array object\n";
          return EXIT_FAILURE;
        }

        const auto& interval = element.va_intervals.front();
        for (const Leaf& leaf : object.leaves) {
          if (!intervalContains(
                  interval, reinterpret_cast<uintptr_t>(&leaf), sizeof(leaf))) {
            std::cerr << "Expected array interval to cover each element\n";
            return EXIT_FAILURE;
          }
        }
      }

      if (typePathContains(element, "leaves") && element.name == "value") {
        ++leafValueFields;
      }
    }

    if (!sawArrayElement) {
      std::cerr << "Expected to find Root::leaves\n";
      return EXIT_FAILURE;
    }

    if (leafValueFields != object.leaves.size()) {
      std::cerr << "Expected traversal to visit each array element\n";
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
