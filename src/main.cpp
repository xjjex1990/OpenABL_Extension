/* Copyright 2017 OpenABL Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. */

#include <chrono>
#include <iostream>
#include "Cli.hpp"
#include "ParserContext.hpp"
#include "Analysis.hpp"
#include "AnalysisVisitor.hpp"
#include "FileUtil.hpp"
#include "backend/Backend.hpp"

namespace OpenABL {

void registerBuiltinFunctions(FunctionList &funcs) {
  funcs.add("dot", "dot_float2", { Type::VEC2, Type::VEC2 }, Type::FLOAT);
  funcs.add("dot", "dot_float3", { Type::VEC3, Type::VEC3 }, Type::FLOAT);
  funcs.add("length", "length_float2", { Type::VEC2 }, Type::FLOAT);
  funcs.add("length", "length_float3", { Type::VEC3 }, Type::FLOAT);
  funcs.add("dist", "dist_float2", { Type::VEC2, Type::VEC2 }, Type::FLOAT);
  funcs.add("dist", "dist_float3", { Type::VEC3, Type::VEC3 }, Type::FLOAT);
  funcs.add("normalize", "normalize_float2", { Type::VEC2 }, Type::VEC2);
  funcs.add("normalize", "normalize_float3", { Type::VEC3 }, Type::VEC3);
  funcs.add("random", "random_float", { Type::FLOAT, Type::FLOAT }, Type::FLOAT);
  funcs.add("randomInt", "random_int", { Type::INT32, Type::INT32 }, Type::INT32);
  funcs.add("MWC64X", "MWC64X", { Type::FLOAT}, Type::FLOAT);

  funcs.add("abs", { Type::FLOAT }, Type::FLOAT);
  funcs.add("sin", { Type::FLOAT }, Type::FLOAT);
  funcs.add("cos", { Type::FLOAT }, Type::FLOAT);
  funcs.add("tan", { Type::FLOAT }, Type::FLOAT);
  funcs.add("sinh", { Type::FLOAT }, Type::FLOAT);
  funcs.add("cosh", { Type::FLOAT }, Type::FLOAT);
  funcs.add("tanh", { Type::FLOAT }, Type::FLOAT);
  funcs.add("asin", { Type::FLOAT }, Type::FLOAT);
  funcs.add("acos", { Type::FLOAT }, Type::FLOAT);
  funcs.add("atan", { Type::FLOAT }, Type::FLOAT);
  funcs.add("atan2", { Type::FLOAT, Type::FLOAT }, Type::FLOAT);
  funcs.add("exp", { Type::FLOAT }, Type::FLOAT);
  funcs.add("log", { Type::FLOAT }, Type::FLOAT);
  funcs.add("sqrt", { Type::FLOAT }, Type::FLOAT);
  funcs.add("cbrt", { Type::FLOAT }, Type::FLOAT);
  funcs.add("round", { Type::FLOAT }, Type::FLOAT);
  funcs.add("pow", { Type::FLOAT, Type::FLOAT }, Type::FLOAT);
  funcs.add("min", { Type::FLOAT, Type::FLOAT }, Type::FLOAT);
  funcs.add("max", { Type::FLOAT, Type::FLOAT }, Type::FLOAT);

  // Agent specific functions
  funcs.add("add", { Type::AGENT }, Type::VOID, FunctionSignature::MAIN_STEP_ONLY);
  funcs.add("removeCurrent", {}, Type::VOID, FunctionSignature::STEP_ONLY);
  funcs.add("near",
    { Type::AGENT, Type::FLOAT },
    { Type::ARRAY, Type::AGENT },
    FunctionSignature::STEP_ONLY);
  funcs.add("on",
    { Type::AGENT },
    { Type::ARRAY, Type::AGENT },
    FunctionSignature::STEP_ONLY);
  funcs.add("save", { Type::STRING }, Type::VOID, FunctionSignature::MAIN_ONLY);

  // Reduction functions
  funcs.add("count", { Type::AGENT_TYPE }, Type::INT32, FunctionSignature::SEQ_STEP_ONLY);
  funcs.add("getLastExecTime", {}, Type::FLOAT, FunctionSignature::SEQ_STEP_ONLY);

  // sumFn() determines the return type based on the member that is summed over
  FunctionSignature sumFn(
      "sum", "sum", { Type::AGENT_MEMBER }, Type::UNRESOLVED,
      FunctionSignature::SEQ_STEP_ONLY, nullptr);
  sumFn.customIsCompatibleWith = [](const std::vector<Type> &argTypes) {
    return argTypes.size() == 1 && argTypes[0].isAgentMember();
  };
  sumFn.customGetConcreteSignature = [sumFn](const std::vector<Type> &argTypes) {
    Type argType = argTypes[0];
    assert(argType.isAgentMember());
    Type memberType = argType.getAgentMember()->type->resolved;

    FunctionSignature copy = sumFn;
    copy.paramTypes = argTypes;
    copy.returnType = memberType;
    if (memberType.isBool()) {
      // Bools sum to integer
      copy.returnType = { Type::INT32 };
    }
    return copy;
  };
  funcs.add(sumFn);

  // count() how often a member has a certain value. The type of the second
  // argument must match the type of the member
  FunctionSignature countFn(
      "count", "count_member", { Type::AGENT_MEMBER, Type::UNRESOLVED }, Type::INT32,
      FunctionSignature::SEQ_STEP_ONLY, nullptr);
  countFn.customIsCompatibleWith = [](const std::vector<Type> &argTypes) {
    if (argTypes.size() != 2 && !argTypes[0].isAgentMember()) {
      return false;
    }

    AST::AgentMember *member = argTypes[0].getAgentMember();
    return argTypes[1].isCompatibleWith(member->type->resolved);
  };
  countFn.customGetConcreteSignature = [countFn](const std::vector<Type> &argTypes) {
    FunctionSignature copy = countFn;
    copy.paramTypes = argTypes;
    return copy;
  };
  funcs.add(countFn);

  // log_csv() is a variadic function. We don't have native support for variadics,
  // implement some custom handlers.
  FunctionSignature logCsvFn(
      "log_csv", "log_csv", {}, Type::VOID, FunctionSignature::SEQ_STEP_ONLY, nullptr);
  logCsvFn.customIsCompatibleWith = [](const std::vector<Type> &argTypes) {
    // For now only support int and float arguments
    for (Type type : argTypes) {
      if (!type.isInt() && !type.isFloat()) {
        return false;
      }
    }
    return true;
  };
  logCsvFn.customGetConcreteSignature = [logCsvFn](const std::vector<Type> &argTypes) {
    FunctionSignature copy = logCsvFn;
    copy.paramTypes = argTypes;
    return copy;
  };
  funcs.add(logCsvFn);
}

std::map<std::string, std::unique_ptr<Backend>> getBackends() {
  std::map<std::string, std::unique_ptr<Backend>> backends;
  backends["c"] = std::unique_ptr<Backend>(new CBackend);
  backends["cl"] = std::unique_ptr<Backend>(new CLBackend);
  backends["flame"] = std::unique_ptr<Backend>(new FlameBackend);
  backends["flamegpu"] = std::unique_ptr<Backend>(new FlameGPUBackend);
  backends["mason"] = std::unique_ptr<Backend>(new MasonBackend);
  backends["dmason"] = std::unique_ptr<Backend>(new DMasonBackend);
  return backends;
}

void printHelp() {
  std::cout << "Usage: ./OpenABL -i input.abl -o ./output-dir -b backend\n\n"
               "Options:\n"
               "  -A, --asset-dir    Asset directory (default: ./asset)\n"
               "  -b, --backend      Backend\n"
               "  -B, --build        Build the generated code\n"
               "  -C, --config       Specify a configuration value (name=value)\n"
               "  -c, --conflict     auto-generate conflict resoultion (only validate for OpenCL backend)\n"
               "  -D, --deps         Deps directory (default: ./deps)\n"
               "  -h, --help         Display this help\n"
               "  -i, --input        Input file\n"
               "  -o, --output-dir   Output directory\n"
               "  -P, --param        Specify a simulation parameter (name=value)\n"
               "  -R, --run          Build and run the generated code\n"
               "\n"
               "Available backends:\n"
               " * c\n"
               " * flame\n"
               " * flamegpu\n"
               " * mason\n"
               " * dmason\n"
               "\n"
               "Available configuration options:\n"
               " * bool use_float (default: false, flame/gpu only)\n"
               " * bool visualize (default: false, d/mason only)\n"
            << std::flush;
}

int main(int argc, char **argv) {
  Cli::Options options;
  try {
    options = Cli::parseOptions(argc, argv);
  } catch (const Cli::OptionError &e) {
    printHelp();
    std::cerr << "\nERROR: " << e.what() << std::endl;
    return 1;
  }

  if (options.help) {
    printHelp();
    return 0;
  }

  if (options.fileName.empty()) {
    return 1;
  }

  FILE *mainFile = fopen(options.fileName.c_str(), "r");
  if (!mainFile) {
    std::cerr << "File \"" << options.fileName << "\" could not be opened." << std::endl;
    return 1;
  }

  if (!directoryExists(options.assetDir)) {
    std::cerr << "Asset directory \"" << options.assetDir << "\" does not exist "
              << "(override with -A or --asset-dir)" << std::endl;
    return 1;
  }

  std::string libFileName = options.assetDir + "/lib.abl";
  FILE *libFile = fopen(libFileName.c_str(), "r");
  if (!libFile) {
    std::cerr << "Library file \"" << libFileName << "\" could not be opened." << std::endl;
    return 1;
  }

  ParserContext mainCtx(mainFile);
  ParserContext libCtx(libFile);
  if (!mainCtx.parse() || !libCtx.parse()) {
    return 1;
  }

  AST::Script &mainScript = *mainCtx.script;
  AST::Script &libScript = *libCtx.script;

  int numErrors = 0;
  ErrorStream err([&numErrors](const Error &err) {
    std::cerr << err.msg << " on line " << err.loc.begin.line << std::endl;
    numErrors++;
  });

  FunctionList funcs;
  registerBuiltinFunctions(funcs);

  AnalysisVisitor visitor(mainScript, options.params, err, funcs, options.backend, options.conflict);
  visitor.handleLibScript(libScript);
  visitor.handleMainScript(mainScript);

  if (numErrors > 0) {
    // There were errors, abort
    return 1;
  }

  if (options.lintOnly) {
    // Linting only, don't try to generate output
    return 0;
  }

  if (!options.outputDir.empty()) {
    createDirectory(options.outputDir);
  } else if (options.build || options.run) {
    options.outputDir = createTemporaryDirectory();
    std::cout << "Writing to directory " << options.outputDir << std::endl;
  } else {
    std::cerr << "Missing output directory (-o or --output-dir)" << std::endl;
    return 1;
  }

  // Make deps dir absolute, as it will be embedded in shell scripts
  options.depsDir = getAbsolutePath(options.depsDir);

  auto backends = getBackends();
  auto it = backends.find(options.backend);
  if (it == backends.end()) {
    std::cerr << "Unknown backend \"" << options.backend << "\"" << std::endl;
    return 1;
  }

  Backend &backend = *it->second;
  BackendContext backendCtx = {
    options.outputDir,
    options.assetDir,
    options.depsDir,
    Config { options.config },
    options.conflict
  };

  try {
    backend.generate(mainScript, backendCtx);
  } catch (const BackendError &e) {
    // The backend does not support a feature.
    // Return a different exit code to distinguish this case.
    std::cerr << e.what() << std::endl;
    return 2;
  } catch (const std::runtime_error &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  if (options.build || options.run) {
    changeWorkingDirectory(options.outputDir);
    backend.initEnv(backendCtx);

    if (!fileExists("./build.sh")) {
      std::cerr << "Build file for this backend not found" << std::endl;
      return 1;
    }

    if (!executeCommand("./build.sh")) {
      std::cerr << "Build failed" << std::endl;
      return 1;
    }

    if (options.run) {
      if (!fileExists("./run.sh")) {
        std::cerr << "Run file for this backend not found" << std::endl;
        return 1;
      }

      auto start = std::chrono::high_resolution_clock::now();
      if (!executeCommand("./run.sh")) {
        std::cerr << "Run failed" << std::endl;
        return 1;
      }
      auto end = std::chrono::high_resolution_clock::now();

      auto msecs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      auto secs = msecs/1000.0;
      std::cout << "Execution time: " << secs << "s" << std::endl;
    }
  }

  fclose(mainFile);
  return 0;
}

}

int main(int argc, char **argv) {
  return OpenABL::main(argc, argv);
}
