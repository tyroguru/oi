#include <oi/oi.h>

#include <iostream>
#include <string>
#include <vector>

struct Simple {
  int id;
  std::string name;
  std::vector<int> values;
};

int main() {
  Simple s{
      .id = 42,
      .name = "hello",
      .values = {1, 2, 3, 4},
  };

  oi::GeneratorOptions opts;
  opts.debugLevel = 0;  // 3 for full debug
  /*
   * A valid config has to be passed to be passed to OIL so that it can locate
   * container definitions and headers. To generate the basic configuration,
   * issue a `./tools/config_gen.py -c clang++ build/testing.oid.toml` from the
   * root of the repo.
   */
  opts.configFilePaths = {"/home/jon/Code/oi/build/testing.oid.toml"};

  auto result = oi::setupAndIntrospect<Simple>(s, opts);
  if (!result) {
    std::cerr << "OIL JIT did not initialise\n";
    return 1;
  }

  std::size_t count = 0;
  for (const auto& element : *result) {
    std::cout << element.name << " static=" << element.static_size
              << " exclusive=" << element.exclusive_size
              << " primitive=" << element.is_primitive << "\n";
    ++count;
  }

  std::cout << "elements=" << count << "\n";
  return 0;
}
