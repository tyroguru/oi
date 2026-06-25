/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "oi/CodeGen.h"
#include "oi/Config.h"
#include "oi/Features.h"
#include "oi/OICodeGen.h"
#include "oi/OICompiler.h"
#include "oi/SymbolService.h"

extern "C" {
#include <drgn.h>
}

namespace fs = std::filesystem;
using namespace oi::detail;

DEFINE_string(binary, "", "ELF binary containing debug information");
DEFINE_string(type, "", "Type name to generate OIL introspection code for");
DEFINE_string(config_file, "", "OID TOML config file");
DEFINE_string(output_source, "", "Optional path for generated C++ source");
DEFINE_string(output_object, "", "Optional path for compiled object code");
DEFINE_bool(compile, false, "Compile generated source to object code");
DEFINE_int32(debug_level, -1, "Verbose logging level");

namespace {

std::map<Feature, bool> defaultFeatures() {
  return {
      {Feature::TypeGraph, true},
      {Feature::TreeBuilderV2, true},
      {Feature::Library, true},
      {Feature::PackStructs, true},
      {Feature::PruneTypeGraph, true},
  };
}

std::vector<fs::path> configFilePaths() {
  std::vector<fs::path> paths;
  if (!FLAGS_config_file.empty()) {
    paths.emplace_back(FLAGS_config_file);
  }
  return paths;
}

fs::path sourcePathForCompile() {
  if (!FLAGS_output_source.empty()) {
    return FLAGS_output_source;
  }
  return "oil_typegen.cpp";
}

void writeGeneratedSource(const fs::path& outputPath, const std::string& code) {
  std::ofstream outputFile(outputPath);
  if (!outputFile) {
    throw std::runtime_error("failed to open generated source output path: " +
                             outputPath.string());
  }

  outputFile << code;
}

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::SetUsageMessage(
      "Generate OIL introspection code for an explicit DWARF type.\n"
      "Example: oil-typegen --binary ./app --type rocksdb::FileMetaData "
      "--config_file build/testing.oid.toml --output_source /tmp/t.cpp "
      "--output_object /tmp/t.o --compile");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  FLAGS_stderrthreshold = 0;
  if (FLAGS_debug_level >= 0) {
    google::LogToStderr();
    google::SetStderrLogging(google::INFO);
    google::SetVLOGLevel("*", FLAGS_debug_level);
    gflags::SetCommandLineOption("minloglevel", "0");
  }

  if (FLAGS_binary.empty()) {
    LOG(ERROR) << "--binary is required";
    return EXIT_FAILURE;
  }
  if (FLAGS_type.empty()) {
    LOG(ERROR) << "--type is required";
    return EXIT_FAILURE;
  }
  if (FLAGS_compile && FLAGS_output_object.empty()) {
    LOG(ERROR) << "--output_object is required when --compile is set";
    return EXIT_FAILURE;
  }

  OICompiler::Config compilerConfig;
  OICodeGen::Config generatorConfig;
  auto configPaths = configFilePaths();
  auto features = config::processConfigFiles(
      configPaths, defaultFeatures(), compilerConfig, generatorConfig);
  if (!features) {
    LOG(ERROR) << "failed to process configuration";
    return EXIT_FAILURE;
  }
  compilerConfig.features = *features;
  generatorConfig.features = *features;

  auto symbols = std::make_shared<SymbolService>(fs::path{FLAGS_binary});
  if (symbols->getDrgnProgram() == nullptr) {
    LOG(ERROR) << "failed to load debug information for " << FLAGS_binary;
    return EXIT_FAILURE;
  }

  auto rootType = symbols->findTypeByName(FLAGS_type);
  if (!rootType) {
    LOG(ERROR) << "failed to find type '" << FLAGS_type << "'";
    return EXIT_FAILURE;
  }

  std::cout << "Resolved type: " << SymbolService::getTypeName(rootType->type)
            << '\n';

  CodeGen codegen{generatorConfig, *symbols};
  std::string code;
  if (!codegen.codegenFromDrgn(rootType->type, code)) {
    LOG(ERROR) << "failed to generate OIL source";
    return EXIT_FAILURE;
  }

  if (!FLAGS_output_source.empty()) {
    writeGeneratedSource(FLAGS_output_source, code);
    std::cout << "Wrote generated source: " << FLAGS_output_source << '\n';
  }

  if (FLAGS_compile) {
    OICompiler compiler{symbols, compilerConfig};
    auto sourcePath = sourcePathForCompile();
    if (!compiler.compile(code, sourcePath, FLAGS_output_object)) {
      LOG(ERROR) << "failed to compile generated source";
      return EXIT_FAILURE;
    }
    std::cout << "Wrote object code: " << FLAGS_output_object << '\n';
  }

  return EXIT_SUCCESS;
}
