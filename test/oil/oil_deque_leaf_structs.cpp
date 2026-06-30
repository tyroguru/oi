// test/oil/oil_deque_leaf_structs.cpp

#include <oi/oi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
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
  std::deque<Leaf> leaves;
};

#ifdef __GLIBCXX__
template <typename T>
struct DequeLayout {
  void* map;
  std::size_t map_capacity;
  typename std::deque<T>::const_iterator start;
  typename std::deque<T>::const_iterator finish;
};
#endif

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

void printDequeIntervals(const oi::result::Element& element) {
  std::cout << "std::deque captured intervals: " << element.va_intervals.size()
            << '\n';

  for (std::size_t i = 0; i < element.va_intervals.size(); ++i) {
    std::cout << "  interval[" << i << "]=";
    printInterval(std::cout, element.va_intervals[i]);
    std::cout << '\n';
  }
}

void printExpectedAddresses(const Root& object) {
  const auto& leaves = object.leaves;

  std::cout << "std::deque object address: "
            << static_cast<const void*>(&leaves) << " size=" << sizeof(leaves)
            << '\n';

#ifdef __GLIBCXX__
  const auto* layout = reinterpret_cast<const DequeLayout<Leaf>*>(&leaves);
  const auto begin = leaves.begin();
  const auto end = leaves.end();
  const auto block_size = reinterpret_cast<uintptr_t>(begin._M_last) -
                          reinterpret_cast<uintptr_t>(begin._M_first);

  std::cout << "std::deque map address:    " << layout->map
            << " size=" << layout->map_capacity * sizeof(void*) << '\n';

  std::size_t block_index = 0;
  for (auto node = begin._M_node; node <= end._M_node; ++node) {
    std::cout << "  block[" << block_index
              << "] address=" << static_cast<const void*>(*node)
              << " size=" << block_size << '\n';
    ++block_index;
  }
#endif

  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const Leaf& leaf = leaves[i];
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

    bool sawDequeElement = false;
    std::size_t leafValueFields = 0;

    for (const auto& element : *result) {
      if (element.name == "leaves") {
        sawDequeElement = true;

        printExpectedAddresses(object);
        printDequeIntervals(element);

        const auto& stats = element.container_stats;
        if (!stats.has_value() || stats->length != object.leaves.size() ||
            stats->capacity < object.leaves.size()) {
          std::cerr << "Unexpected std::deque container stats\n";
          return EXIT_FAILURE;
        }

#ifdef __GLIBCXX__
        const auto& leaves = object.leaves;
        const auto* layout =
            reinterpret_cast<const DequeLayout<Leaf>*>(&leaves);
        const auto begin = leaves.begin();
        const auto end = leaves.end();
        const auto block_count = static_cast<std::size_t>(
            1 + std::distance(begin._M_node, end._M_node));
        const auto block_size = reinterpret_cast<uintptr_t>(begin._M_last) -
                                reinterpret_cast<uintptr_t>(begin._M_first);
        const std::size_t expected_intervals = 2 + block_count;

        if (element.va_intervals.size() != expected_intervals) {
          std::cerr << "Expected deque object, map, and one VA interval per "
                       "allocated block\n";
          return EXIT_FAILURE;
        }

        if (!hasExactInterval(element,
                              reinterpret_cast<uintptr_t>(&object.leaves),
                              sizeof(object.leaves))) {
          std::cerr << "Expected VA interval for std::deque object\n";
          return EXIT_FAILURE;
        }

        if (!hasExactInterval(element,
                              reinterpret_cast<uintptr_t>(layout->map),
                              layout->map_capacity * sizeof(void*))) {
          std::cerr << "Expected VA interval for std::deque map storage\n";
          return EXIT_FAILURE;
        }

        for (auto node = begin._M_node; node <= end._M_node; ++node) {
          if (!hasExactInterval(
                  element, reinterpret_cast<uintptr_t>(*node), block_size)) {
            std::cerr << "Expected VA interval for each std::deque block\n";
            return EXIT_FAILURE;
          }
        }
#else
        if (element.va_intervals.empty()) {
          std::cerr << "Expected std::deque VA intervals\n";
          return EXIT_FAILURE;
        }
#endif

        for (const Leaf& leaf : object.leaves) {
          if (!hasSingleCoveringInterval(
                  element, reinterpret_cast<uintptr_t>(&leaf), sizeof(leaf))) {
            std::cerr << "Expected one block VA interval covering each deque "
                         "element\n";
            return EXIT_FAILURE;
          }
        }
      }

      if (typePathContains(element, "leaves") && element.name == "value") {
        ++leafValueFields;
      }
    }

    if (!sawDequeElement) {
      std::cerr << "Expected to find Root::leaves\n";
      return EXIT_FAILURE;
    }

    if (leafValueFields != object.leaves.size()) {
      std::cerr << "Expected traversal to visit each deque element\n";
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
