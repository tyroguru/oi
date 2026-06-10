#include <oi/oi.h>

#include <cstdlib>
#include <exception>
#include <iostream>

#ifndef OIL_TEST_CONFIG_PATH
#error "OIL_TEST_CONFIG_PATH must be defined"
#endif

namespace {

struct Inner {
  std::uint32_t value;
  bool active;
};

struct Outer {
  std::uint64_t id;
  Inner inner;
};

}  // namespace

int main() {
  try {
    Outer object{
        .id = 123,
        .inner =
            {
                .value = 42,
                .active = true,
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

    bool sawId = false;
    bool sawInner = false;
    bool sawValue = false;
    bool sawActive = false;

    for (const auto& element : *result) {
      std::cout << "name=\"" << element.name << "\""
                << " static_size=" << element.static_size
                << " exclusive_size=" << element.exclusive_size << '\n';

      if (element.name == "id") {
        sawId = true;
      } else if (element.name == "inner") {
        sawInner = true;
      } else if (element.name == "value") {
        sawValue = true;
      } else if (element.name == "active") {
        sawActive = true;
      }
    }

    if (!sawId || !sawInner || !sawValue || !sawActive) {
      std::cerr << "Expected to see id, inner, value, and active\n"
                << "sawId=" << sawId << " sawInner=" << sawInner
                << " sawValue=" << sawValue << " sawActive=" << sawActive
                << '\n';
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
