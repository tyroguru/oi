#include <oi/oi.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#ifndef OIL_TEST_CONFIG_PATH
#error "OIL_TEST_CONFIG_PATH must be defined"
#endif

namespace {

struct PlainStruct {
  std::uint32_t id;
  std::uint64_t count;
  bool enabled;
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

void printElementData(const oi::result::Element& element) {
  std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;

        if constexpr (std::is_same_v<Value, std::nullopt_t>) {
          std::cout << "none";
        } else if constexpr (std::is_same_v<Value,
                                            oi::result::Element::Pointer>) {
          std::cout << "pointer=0x" << std::hex << value.p << std::dec;
        } else if constexpr (std::is_same_v<Value,
                                            oi::result::Element::Scalar>) {
          std::cout << "scalar=" << value.n;
        } else if constexpr (std::is_same_v<Value, std::string>) {
          std::cout << "string=\"" << value << '"';
        }
      },
      element.data);
}

void printElement(const oi::result::Element& element) {
  std::cout << "name=\"" << element.name << "\""
            << " static_size=" << element.static_size
            << " exclusive_size=" << element.exclusive_size
            << " primitive=" << std::boolalpha << element.is_primitive;

  if (element.pointer.has_value()) {
    std::cout << " address=0x" << std::hex << *element.pointer << std::dec;
  }

  std::cout << " type_path=";
  printStringViewSpan(element.type_path);

  std::cout << " type_names=";
  printStringViewSpan(element.type_names);

  std::cout << " data=";
  printElementData(element);

  std::cout << '\n';
}

}  // namespace

int main() {
  try {
    PlainStruct object{
        .id = 42,
        .count = 123456789,
        .enabled = true,
    };

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
    bool sawId = false;
    bool sawCount = false;
    bool sawEnabled = false;

    for (const auto& element : *result) {
      ++elementCount;
      printElement(element);

      if (element.name == "id") {
        sawId = true;
      } else if (element.name == "count") {
        sawCount = true;
      } else if (element.name == "enabled") {
        sawEnabled = true;
      }
    }

    std::cout << "element_count=" << elementCount << '\n';

    if (!sawId || !sawCount || !sawEnabled) {
      std::cerr << "Expected to see fields: id, count, enabled\n"
                << "sawId=" << std::boolalpha << sawId
                << " sawCount=" << sawCount << " sawEnabled=" << sawEnabled
                << '\n';
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
