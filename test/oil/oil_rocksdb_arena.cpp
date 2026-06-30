// test/oil/oil_rocksdb_arena.cpp

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
#include <vector>

#include "memory/arena.h"

#ifndef OIL_TEST_CONFIG_PATH
#error "OIL_TEST_CONFIG_PATH must be defined"
#endif

#ifndef OIL_TEST_SOURCE_DIR
#error "OIL_TEST_SOURCE_DIR must be defined"
#endif

namespace {

struct Root {
  rocksdb::Arena arena;
};

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

void printArenaIntervals(const oi::result::Element& element) {
  std::cout << "rocksdb::Arena captured intervals: "
            << element.va_intervals.size() << '\n';

  for (std::size_t i = 0; i < element.va_intervals.size(); ++i) {
    std::cout << "  interval[" << i << "]=";
    printInterval(std::cout, element.va_intervals[i]);
    std::cout << '\n';
  }
}

std::filesystem::path writeExtraConfig() {
  const auto path =
      std::filesystem::temp_directory_path() / "oil-rocksdb-arena-test.toml";

  std::ofstream out(path);
  out << "[headers]\n"
      << "user_paths = [\"" << OIL_TEST_SOURCE_DIR << "\"]\n";

  return path;
}

bool pointerInObject(const Root& object, const char* ptr) {
  const auto object_begin = reinterpret_cast<uintptr_t>(&object.arena);
  const auto object_end = object_begin + sizeof(object.arena);
  const auto addr = reinterpret_cast<uintptr_t>(ptr);
  return object_begin <= addr && addr < object_end;
}

uintptr_t fillArenaAndFindRegularBlock(Root& object) {
  constexpr std::size_t kAllocationSize = 128;
  std::vector<char*> allocations;

  for (std::size_t i = 0; i < 40; ++i) {
    char* ptr = object.arena.Allocate(kAllocationSize);
    allocations.push_back(ptr);

    if (!pointerInObject(object, ptr)) {
      return reinterpret_cast<uintptr_t>(ptr) + kAllocationSize -
             object.arena.BlockSize();
    }
  }

  return 0;
}

void printExpectedAddresses(const Root& object, uintptr_t regular_block) {
  std::cout << "rocksdb::Arena object address: "
            << static_cast<const void*>(&object.arena)
            << " size=" << sizeof(object.arena) << '\n';
  std::cout << "rocksdb::Arena regular block address: " << std::showbase
            << std::hex << regular_block << std::dec << std::noshowbase
            << " size=" << object.arena.BlockSize() << '\n';
}

}  // namespace

int main() {
  try {
    Root object;
    const uintptr_t regularBlock = fillArenaAndFindRegularBlock(object);
    if (regularBlock == 0) {
      std::cerr << "Expected Arena allocation to leave inline storage\n";
      return EXIT_FAILURE;
    }

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

    bool sawArena = false;
    bool sawObjectInterval = false;
    bool sawRegularBlockInterval = false;

    for (const auto& element : *result) {
      if (element.name == "arena") {
        sawArena = true;

        printExpectedAddresses(object, regularBlock);
        printArenaIntervals(element);

        const auto& stats = element.container_stats;
        if (!stats.has_value() ||
            stats->capacity != object.arena.MemoryAllocatedBytes() ||
            stats->length != object.arena.ApproximateMemoryUsage()) {
          std::cerr << "Unexpected rocksdb::Arena container stats\n";
          return EXIT_FAILURE;
        }

        sawObjectInterval =
            hasExactInterval(element,
                             reinterpret_cast<uintptr_t>(&object.arena),
                             sizeof(object.arena));

        sawRegularBlockInterval =
            hasExactInterval(element, regularBlock, object.arena.BlockSize());
      }
    }

    bool ok = true;

    if (!sawArena) {
      std::cerr << "Expected to find Root::arena\n";
      ok = false;
    }

    if (!sawObjectInterval) {
      std::cerr << "Expected rocksdb::Arena object VA interval\n";
      ok = false;
    }

    if (!sawRegularBlockInterval) {
      std::cerr << "Expected rocksdb::Arena regular block VA interval\n";
      ok = false;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
