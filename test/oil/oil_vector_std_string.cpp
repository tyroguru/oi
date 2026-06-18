// test/oil/oil_vector_std_string.cpp

#include <oi/oi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef OIL_TEST_CONFIG_PATH
#error "OIL_TEST_CONFIG_PATH must be defined"
#endif

namespace {

struct Root {
  std::uint64_t id;
  std::vector<std::string> strings;
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

void printStringsStorage(const Root& object) {
  const auto& strings = object.strings;

  std::cout << "Root object address:            "
            << static_cast<const void*>(&object) << '\n';

  std::cout << "Root::strings vector address:   "
            << static_cast<const void*>(&strings) << '\n';

  std::cout << "Root::strings.data() address:   "
            << static_cast<const void*>(strings.data()) << '\n';

  std::cout << "Root::strings size:             " << strings.size() << '\n'
            << "Root::strings capacity:         " << strings.capacity() << '\n'
            << "sizeof(std::string):            " << sizeof(std::string)
            << '\n';

  for (std::size_t i = 0; i < strings.size(); ++i) {
    const std::string& string = strings[i];

    std::cout << "strings[" << i << "]"
              << " object_address=" << static_cast<const void*>(&string)
              << " data_address=" << static_cast<const void*>(string.data())
              << " size=" << string.size() << " capacity=" << string.capacity()
              << " value=\"" << string << "\"\n";
  }
}

}  // namespace

int main() {
  try {
    Root object{
        .id = 123,
        .strings =
            {
                "Sed ut",
                "Sed ut perspiciatis",
                "Sed ut perspiciatis unde omnis iste natus error sit voluptat",
                "Sed",
                "Sed ut perspiciatis unde omnis iste natus error sit "
                "voluptatem accusantium doloremque laudantium, totam rem "
                "aperiam",
                "Sed ut perspiciatis",
                "Sed ut perspiciatis unde omnis iste natus error sit "
                "voluptatem accusantium doloremque laudantium, totam "
                "rem aperiam, eaque ipsa quae ab illo inventore veritatis "
                "et quasi architecto beatae vitae dicta sunt explicabo",
                "Finis",
            },
    };

    printStringsStorage(object);

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
    std::size_t stringObjectCount = 0;
    std::size_t stringDataIntervalCount = 0;

    bool sawId = false;
    bool sawStrings = false;
    bool sawStringsContainerStats = false;
    bool sawCorrectStringsLength = false;
    bool sawPlausibleStringsCapacity = false;

    for (const auto& element : *result) {
      ++elementCount;
      printElement(element);

      if (element.name == "id") {
        sawId = true;
      }

      if (element.name == "strings") {
        sawStrings = true;

        if (element.container_stats.has_value()) {
          sawStringsContainerStats = true;
          sawCorrectStringsLength =
              element.container_stats->length == object.strings.size();
          sawPlausibleStringsCapacity =
              element.container_stats->capacity >= object.strings.size();
        }
      }

      const bool underStrings = typePathContains(element, "strings");

      if (underStrings && element.static_size == sizeof(std::string) &&
          !element.is_primitive) {
        ++stringObjectCount;
      }

      /*
       * This is intentionally loose because the exact element name used for
       * std::string's character storage may depend on your new interval
       * implementation.
       *
       * The important property for this test is that some std::string elements
       * below Root::strings carry VA intervals beyond the std::string object
       * itself, corresponding to externally allocated string storage.
       */
      if (underStrings && !element.va_intervals.empty()) {
        for (const auto& interval : element.va_intervals) {
          for (const auto& string : object.strings) {
            const auto data = reinterpret_cast<std::uintptr_t>(string.data());

            if (interval.base == data && interval.size >= string.size()) {
              ++stringDataIntervalCount;
            }
          }
        }
      }
    }

    std::cout << "element_count=" << elementCount << '\n'
              << "string_object_count=" << stringObjectCount << '\n'
              << "string_data_interval_count=" << stringDataIntervalCount
              << '\n';

    bool ok = true;

    if (!sawId) {
      std::cerr << "Expected to see Root::id\n";
      ok = false;
    }

    if (!sawStrings) {
      std::cerr << "Expected to see Root::strings\n";
      ok = false;
    }

    if (!sawStringsContainerStats) {
      std::cerr << "Expected vector field to have container stats\n";
      ok = false;
    }

    if (!sawCorrectStringsLength) {
      std::cerr << "Expected vector length to be " << object.strings.size()
                << '\n';
      ok = false;
    }

    if (!sawPlausibleStringsCapacity) {
      std::cerr << "Expected vector capacity to be at least "
                << object.strings.size() << '\n';
      ok = false;
    }

    if (stringObjectCount != object.strings.size()) {
      std::cerr << "Expected to see " << object.strings.size()
                << " std::string object elements, saw " << stringObjectCount
                << '\n';
      ok = false;
    }

    /*
     * The fixture has three deliberately long strings. They should be longer
     * than both common libstdc++ SSO capacity and common libc++ SSO capacity.
     */
    if (stringDataIntervalCount < 3) {
      std::cerr << "Expected to see at least 3 std::string data intervals, saw "
                << stringDataIntervalCount << '\n';
      ok = false;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
