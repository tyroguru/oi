// test/oil/oil_rocksdb_autovector.cpp

#include <oi/oi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string_view>

#include "util/autovector.h"

#ifndef OIL_TEST_CONFIG_PATH
#error "OIL_TEST_CONFIG_PATH must be defined"
#endif

#ifndef OIL_TEST_SOURCE_DIR
#error "OIL_TEST_SOURCE_DIR must be defined"
#endif

namespace {

struct Leaf {
  std::uint32_t value;
  bool enabled;
};

struct Root {
  rocksdb::autovector<Leaf, 2> leaves;
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

void printInterval(std::ostream& out,
                   const oi::result::Element::VAInterval& interval) {
  out << "[" << std::showbase << std::hex << interval.base << ", "
      << interval.base + interval.size << std::dec << std::noshowbase
      << ") size=" << interval.size;
}

void printAutovectorIntervals(const oi::result::Element& element) {
  std::cout << "autovector captured intervals: " << element.va_intervals.size()
            << '\n';

  for (std::size_t i = 0; i < element.va_intervals.size(); ++i) {
    std::cout << "  interval[" << i << "]=";
    printInterval(std::cout, element.va_intervals[i]);
    std::cout << '\n';
  }
}

void printExpectedAddresses(const Root& object) {
  std::cout << "autovector object address: "
            << static_cast<const void*>(&object.leaves)
            << " size=" << sizeof(object.leaves) << '\n';

  for (std::size_t i = 0; i < object.leaves.size(); ++i) {
    const Leaf& leaf = object.leaves[i];
    std::cout << "  leaf[" << i
              << "] address=" << static_cast<const void*>(&leaf)
              << " size=" << sizeof(leaf) << '\n';
  }
}

std::filesystem::path writeExtraConfig() {
  const auto path =
      std::filesystem::temp_directory_path() / "oil-autovector-test.toml";

  std::ofstream out(path);
  out << "[headers]\n"
      << "user_paths = [\"" << OIL_TEST_SOURCE_DIR << "\"]\n";

  return path;
}

void fillLeaves(Root& object) {
  object.leaves.reserve(7);
  for (std::size_t i = 0; i < 5; ++i) {
    object.leaves.push_back(Leaf{
        .value = static_cast<std::uint32_t>(100 + i),
        .enabled = (i % 2) == 0,
    });
  }
}

}  // namespace

int main() {
  try {
    Root object;
    fillLeaves(object);

    const auto extraConfig = writeExtraConfig();

    oi::GeneratorOptions opts;
    opts.debugLevel = 0;
    opts.configFilePaths.emplace_back(OIL_TEST_CONFIG_PATH);
    opts.configFilePaths.emplace_back(extraConfig);

    const auto result = oi::setupAndIntrospect(object, opts);
    if (!result.has_value()) {
      std::cerr << "OIL introspection did not complete\n";
      return EXIT_FAILURE;
    }

    bool sawLeaves = false;
    bool sawObjectInterval = false;
    bool sawOverflowInterval = false;
    std::size_t leafObjects = 0;
    std::size_t leafValueFields = 0;
    std::size_t leafEnabledFields = 0;

    for (const auto& element : *result) {
      if (element.name == "leaves") {
        sawLeaves = true;

        printExpectedAddresses(object);
        printAutovectorIntervals(element);

        const auto& stats = element.container_stats;
        if (!stats.has_value() || stats->length != object.leaves.size() ||
            stats->capacity != object.leaves.capacity()) {
          std::cerr << "Unexpected autovector container stats\n";
          return EXIT_FAILURE;
        }

        sawObjectInterval =
            hasExactInterval(element,
                             reinterpret_cast<uintptr_t>(&object.leaves),
                             sizeof(object.leaves));

        sawOverflowInterval =
            hasExactInterval(element,
                             reinterpret_cast<uintptr_t>(&object.leaves[2]),
                             (object.leaves.capacity() - 2) * sizeof(Leaf));
      }

      const bool underLeaves = typePathContains(element, "leaves");

      if (underLeaves && element.static_size == sizeof(Leaf) &&
          !element.is_primitive) {
        ++leafObjects;
      }

      if (underLeaves && element.name == "value") {
        ++leafValueFields;
      }

      if (underLeaves && element.name == "enabled") {
        ++leafEnabledFields;
      }
    }

    bool ok = true;

    if (!sawLeaves) {
      std::cerr << "Expected to find Root::leaves\n";
      ok = false;
    }

    if (!sawObjectInterval) {
      std::cerr << "Expected autovector object VA interval\n";
      ok = false;
    }

    if (!sawOverflowInterval) {
      std::cerr << "Expected autovector overflow storage VA interval\n";
      ok = false;
    }

    if (leafObjects != object.leaves.size()) {
      std::cerr << "Expected " << object.leaves.size()
                << " Leaf object elements, saw " << leafObjects << '\n';
      ok = false;
    }

    if (leafValueFields != object.leaves.size()) {
      std::cerr << "Expected traversal to visit each Leaf::value field\n";
      ok = false;
    }

    if (leafEnabledFields != object.leaves.size()) {
      std::cerr << "Expected traversal to visit each Leaf::enabled field\n";
      ok = false;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
