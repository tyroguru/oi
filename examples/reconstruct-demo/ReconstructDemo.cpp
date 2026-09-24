//
// Research prototype (see docs/object-capture-initial-thoughts.md, not part
// of this repo) - a two-process demo proving byte-accurate object capture on
// one process and reconstruction on another. One executable, two modes:
//
//   reconstruct_demo --mode client [--transport local]
//   reconstruct_demo --mode server [--transport local]
//
// The client builds a fixed, human-recognizable DemoObject, captures it via
// oi::introspect() (with the capture-bytes feature enabled - see this
// target's oilgen config), and writes the raw byte stream to a known local
// path. The server polls that path, and once it sees data, reconstructs a
// DemoObject via oi::reconstruct<DemoObject>() and prints it - so a viewer
// can compare the client's "original object" printout against the server's
// "reconstructed object" printout and see they match, even though the
// server process never shared memory with the client and only ever saw a
// flat byte stream.
//
// Only --transport local is implemented (a known filename under /tmp) -
// --transport remote is accepted on the command line but rejected at
// runtime, left as a placeholder for a real network transport later.
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr std::string_view kLocalTransportPath = "/tmp/oi_reconstruct_demo.bytes";
}  // namespace

// The classic "network union" trick: the same 4 bytes viewed either as one
// 32-bit value or as 4 individual octets, so an IPv4 address can be built
// octet-by-octet (or parsed off the wire) and printed in dotted-decimal
// form without ever needing to know or care which view the surrounding
// code actually populated - exactly the class of union OI now treats as an
// opaque, trivially-copyable byte blob (see
// docs/object-capture-initial-thoughts.md's union section, not part of
// this repo): capture doesn't need to know it's an IP address, only that
// it's 4 raw bytes.
union IPv4Address {
  std::uint32_t whole;
  std::uint8_t octets[4];
};

IPv4Address makeIPv4(std::uint8_t a,
                     std::uint8_t b,
                     std::uint8_t c,
                     std::uint8_t d) {
  IPv4Address addr{};
  addr.octets[0] = a;
  addr.octets[1] = b;
  addr.octets[2] = c;
  addr.octets[3] = d;
  return addr;
}

// A plain nested (non-union) struct member - reconstructed member-by-member
// via CodeGen::emitReconstructClassValue's recursion, the same way the
// outer DemoObject itself is. Deliberately not flattened into DemoObject
// directly, so the demo actually exercises the recursive case rather than
// just a flat struct of scalars.
struct Version {
  std::int32_t major;
  std::int32_t minor;
  std::int32_t patch;
};

// External linkage required: DemoObject is a template argument to the weak
// introspectImpl<T>/reconstructImpl<T> symbols oilgen fills in below (see
// include/oi/oi.h) - a type declared in an anonymous namespace has no
// linkage at all, which Clang correctly rejects.
//
// Deliberately covers every member kind reconstruction supports today - a
// nested-enum member, a scalar, a string, a vector, a map (including a
// container-typed map key), a trivially-copyable union, and a nested
// (non-union) struct - so this one object exercises the full,
// currently-supported surface. Deliberately does NOT include a pointer
// member - that's not supported by reconstruction yet (see
// docs/object-capture-initial-thoughts.md's "remaining reconstruction
// gap").
struct DemoObject {
  struct Status {
    enum Enum { PENDING, ACTIVE, DONE };
  };

  Status::Enum status;
  std::int32_t id;
  std::string label;
  std::vector<std::int32_t> scores;
  std::map<std::string, std::int32_t> attributes;
  IPv4Address address;
  Version version;
};

const char* statusName(DemoObject::Status::Enum status) {
  switch (status) {
    case DemoObject::Status::PENDING:
      return "PENDING";
    case DemoObject::Status::ACTIVE:
      return "ACTIVE";
    case DemoObject::Status::DONE:
      return "DONE";
  }
  return "UNKNOWN";
}

void printDemoObject(const DemoObject& object, std::ostream& os) {
  os << "  status = " << statusName(object.status) << '\n';
  os << "  id = " << object.id << '\n';
  os << "  label = \"" << object.label << "\"\n";

  os << "  scores = [";
  for (std::size_t i = 0; i < object.scores.size(); ++i) {
    if (i != 0) {
      os << ", ";
    }
    os << object.scores[i];
  }
  os << "]\n";

  os << "  attributes = {";
  bool first = true;
  for (const auto& [key, value] : object.attributes) {
    if (!first) {
      os << ", ";
    }
    first = false;
    os << key << ": " << value;
  }
  os << "}\n";

  os << "  address = " << static_cast<int>(object.address.octets[0]) << '.'
     << static_cast<int>(object.address.octets[1]) << '.'
     << static_cast<int>(object.address.octets[2]) << '.'
     << static_cast<int>(object.address.octets[3]) << " (same bytes as raw=0x"
     << std::hex << object.address.whole << std::dec << ")\n";

  os << "  version = " << object.version.major << '.' << object.version.minor
     << '.' << object.version.patch << '\n';
}

DemoObject buildDemoObject() {
  return DemoObject{
      .status = DemoObject::Status::ACTIVE,
      .id = 42,
      .label = "Hello from the OI byte-accurate reconstruction demo!",
      .scores = {10, 20, 30, 40, 50},
      .attributes = {{"alpha", 1}, {"beta", 2}, {"gamma", 3}},
      .address = makeIPv4(8, 8, 8, 8),
      .version = Version{.major = 2, .minor = 1, .patch = 0},
  };
}

// Writes via a temp path + rename on the same filesystem, so a rename is
// the only externally-visible filesystem operation - the server's polling
// loop (see waitForBytes) can never observe a partially-written file, which
// a plain "open and write in place" easily could.
void send(std::span<const uint8_t> bytes, const std::string& transport) {
  if (transport != "local") {
    throw std::runtime_error("send: unsupported --transport \"" + transport +
                              "\" (only \"local\" is implemented so far)");
  }

  const std::string path{kLocalTransportPath};
  const std::string tmpPath = path + ".tmp";

  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw std::runtime_error("send: failed to open " + tmpPath);
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }

  std::filesystem::rename(tmpPath, path);
}

// Wakes up every 5 seconds and checks whether the known path has any
// content yet. Deliberately simple and, yes, racy in general (a reader
// could in principle observe a size between 0 and the final size if
// something other than send()'s rename() above ever wrote here) - this is
// a research prototype demonstrating the capture/reconstruct round trip,
// not a transport protocol.
std::vector<std::uint8_t> waitForBytes(const std::string& transport) {
  if (transport != "local") {
    throw std::runtime_error("waitForBytes: unsupported --transport \"" +
                              transport +
                              "\" (only \"local\" is implemented so far)");
  }

  const std::string path{kLocalTransportPath};
  std::cout << "server: waiting for " << path << " ...\n";

  for (;;) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }

  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
}

void printUsageAndExit(std::string_view argv0) {
  std::cerr << "usage: " << argv0
            << " --mode client|server [--transport local]\n";
  std::exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
  std::string mode;
  std::string transport = "local";

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--mode" && i + 1 < argc) {
      mode = argv[++i];
    } else if (arg == "--transport" && i + 1 < argc) {
      transport = argv[++i];
    } else {
      printUsageAndExit(argv[0]);
    }
  }

  if (mode != "client" && mode != "server") {
    printUsageAndExit(argv[0]);
  }

  try {
    if (mode == "client") {
      const DemoObject object = buildDemoObject();
      std::cout << "client: original object:\n";
      printDemoObject(object, std::cout);

      const auto result = oi::introspect(object);
      const auto bytes = result.rawBytes();
      std::cout << "client: captured " << bytes.size()
                << " bytes, sending via transport=" << transport << '\n';

      send(bytes, transport);
      std::cout << "client: done\n";
    } else {
      const auto bytes = waitForBytes(transport);
      std::cout << "server: received " << bytes.size()
                << " bytes, reconstructing...\n";

      const DemoObject reconstructed = oi::reconstruct<DemoObject>(bytes);
      std::cout << "server: reconstructed object:\n";
      printDemoObject(reconstructed, std::cout);
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
