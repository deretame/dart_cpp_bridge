import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:yaml/yaml.dart';

import 'commands.dart';

/// `dcb_gen_tool init --name <lib_name>`
///
/// Scaffolds a minimal dart_cpp_bridge project in the current directory:
///   - dart_cpp_bridge.yaml
///   - CMakeLists.txt
///   - native/api/bridge_api.h
///
/// Then runs `generate` once to produce initial wire code.
Future<int> cmdInit(
  List<String> args, {
  required bool force,
  required bool quiet,
}) async {
  final log = CliLogger(quiet: quiet);

  // Parse --name from args.
  String? libName;
  for (var i = 0; i < args.length; i++) {
    if (args[i] == '--name' && i + 1 < args.length) {
      libName = args[i + 1];
      i++;
    } else if (args[i].startsWith('--name=')) {
      libName = args[i].substring('--name='.length);
    }
  }

  final cwd = Directory.current.path;

  // If --name is not given, try to read from existing pubspec.yaml.
  // The dart_package in yaml MUST match pubspec name for Native Assets
  // asset IDs to resolve correctly at runtime.
  final pubspecName = _readPubspecName(cwd);
  if (libName == null || libName.isEmpty) {
    if (pubspecName != null) {
      libName = pubspecName;
      log.info('Using package name from pubspec.yaml: $libName');
    } else {
      stderr.writeln('error: --name is required (no pubspec.yaml found).');
      stderr.writeln('Usage: dcb_gen_tool init --name <library_name>');
      return 1;
    }
  } else if (pubspecName != null && pubspecName != libName) {
    log.warn('--name "$libName" differs from pubspec.yaml name "$pubspecName". '
        'The dart_package in dart_cpp_bridge.yaml must match your pubspec name '
        'for Native Assets to resolve correctly. Using pubspec name instead.');
    libName = pubspecName;
  }

  // Validate name (CMake target name: alphanumeric + underscore).
  if (!RegExp(r'^[a-zA-Z_][a-zA-Z0-9_]*$').hasMatch(libName)) {
    stderr.writeln(
        'error: invalid library name "$libName". '
        'Use only letters, digits, and underscores (must start with a letter).');
    return 1;
  }

  // Only hard-block on the codegen config itself (would silently overwrite
  // user's existing configuration). Other files get a warning + skip.
  if (File(p.join(cwd, 'dart_cpp_bridge.yaml')).existsSync()) {
    stderr.writeln('error: dart_cpp_bridge.yaml already exists in $cwd.');
    stderr.writeln('Remove it first or edit it manually.');
    return 1;
  }

  // For existing projects, skip files that already exist instead of aborting.
  final skipped = <String>[];
  final hasCmake = File(p.join(cwd, 'native', 'CMakeLists.txt')).existsSync();
  final hasApiDir = Directory(p.join(cwd, 'native', 'api')).existsSync();
  final hasImplDir = Directory(p.join(cwd, 'native', 'api_impl')).existsSync();
  final hasHook = File(p.join(cwd, 'hook', 'build.dart')).existsSync();
  if (hasCmake) skipped.add('native/CMakeLists.txt');
  if (hasApiDir) skipped.add('native/api/');
  if (hasImplDir) skipped.add('native/api_impl/');
  if (hasHook) skipped.add('hook/build.dart');

  log.info('Initializing dart_cpp_bridge project "$libName" in $cwd ...');
  if (skipped.isNotEmpty) {
    log.info('  (skipping existing: ${skipped.join(', ')})');
  }

  // --- Generate files ---
  // NOTE: include_paths only contains project-relative paths. The
  // dart_cpp_bridge native/include directory is resolved automatically
  // at codegen time from .dart_tool/package_config.json (parse_api.py).
  _writeFile(p.join(cwd, 'dart_cpp_bridge.yaml'), _yamlTemplate(libName));
  log.info('  created dart_cpp_bridge.yaml');

  if (!hasCmake) {
    final nativeDir = Directory(p.join(cwd, 'native'));
    if (!nativeDir.existsSync()) nativeDir.createSync(recursive: true);
    _writeFile(
        p.join(nativeDir.path, 'CMakeLists.txt'), _cmakeTemplate(libName));
    log.info('  created native/CMakeLists.txt');
  }

  if (!hasHook) {
    final hookDir = Directory(p.join(cwd, 'hook'));
    if (!hookDir.existsSync()) hookDir.createSync(recursive: true);
    _writeFile(p.join(hookDir.path, 'build.dart'), _hookTemplate(libName));
    log.info('  created hook/build.dart');
  }

  if (!hasApiDir) {
    final apiDir = Directory(p.join(cwd, 'native', 'api'));
    apiDir.createSync(recursive: true);
    _writeFile(p.join(apiDir.path, 'bridge_api.h'), _headerTemplate(libName));
    log.info('  created native/api/bridge_api.h');
  }

  if (!hasImplDir) {
    final implDir = Directory(p.join(cwd, 'native', 'api_impl'));
    implDir.createSync(recursive: true);
    _writeFile(
        p.join(implDir.path, 'bridge_api.cpp'), _implTemplate(libName));
    log.info('  created native/api_impl/bridge_api.cpp');
  }

  // --- Run generate ---
  log.info('Running initial codegen ...');
  final configPath = p.join(cwd, 'dart_cpp_bridge.yaml');
  final exitCode = await runGenerate(
    [configPath],
    force: force,
    quiet: quiet,
    skipVersionCheck: false,
  );
  if (exitCode != 0) return exitCode;

  log.info('Done! Next steps:');
  log.info('  1. Ensure dart_cpp_bridge is in pubspec.yaml && run "dart pub get"');
  log.info('  2. Edit native/api/bridge_api.h + native/api_impl/bridge_api.cpp');
  log.info('  3. Run "dcb_gen_tool generate dart_cpp_bridge.yaml" after API changes');
  log.info('  4. Run "dart run" or "flutter run" (hook builds native lib)');
  if (hasCmake) {
    log.info('  note: existing native/CMakeLists.txt was kept — make sure it');
    log.info('        adds generated/wire_dispatch.cpp to your build target.');
  }
  return 0;
}

/// Reads the `name` field from pubspec.yaml in [cwd], or null if not found.
String? _readPubspecName(String cwd) {
  final pubspecFile = File(p.join(cwd, 'pubspec.yaml'));
  if (!pubspecFile.existsSync()) return null;
  try {
    final content = loadYaml(pubspecFile.readAsStringSync());
    if (content is YamlMap && content['name'] is String) {
      return content['name'] as String;
    }
  } catch (_) {}
  return null;
}

void _writeFile(String path, String content) {
  File(path).writeAsStringSync(content);
}

// ===========================================================================
// Templates
// ===========================================================================

String _yamlTemplate(String libName) {
  return '''
# dart_cpp_bridge codegen config for $libName
dart_package: $libName
cpp_root: native/

scan:
  - native/api/

# Project-relative include paths only.
# dart_cpp_bridge native/include is auto-resolved from package_config.json
# at codegen time — no need to add it here.
include_paths:
  - native
  - native/api

dart_output: lib/src/native_gen/
cpp_wire_output: native/generated/

# clang-format candidate paths (tried top-to-bottom, then PATH fallback).
# clang_format:
#   - C:\\Program Files\\LLVM\\bin

std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
''';
}

String _cmakeTemplate(String libName) => '''
cmake_minimum_required(VERSION 3.24)
project($libName LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Find dart_cpp_bridge (reads .dart_tool/package_config.json) ---
file(READ "\${CMAKE_CURRENT_SOURCE_DIR}/../.dart_tool/package_config.json" _j)
string(JSON _c LENGTH "\${_j}" "packages")
math(EXPR _e "\${_c} - 1")
foreach(_i RANGE \${_e})
  string(JSON _n GET "\${_j}" "packages" \${_i} "name")
  if(_n STREQUAL "dart_cpp_bridge")
    string(JSON _u GET "\${_j}" "packages" \${_i} "rootUri")
    break()
  endif()
endforeach()
if(NOT DEFINED _u)
  message(FATAL_ERROR "dart_cpp_bridge not in package_config.json")
endif()
if(_u MATCHES "^file:///")
  string(REGEX REPLACE "^file:///" "" DCB_PKG_PATH "\${_u}")
elseif(_u MATCHES "^file://")
  string(REGEX REPLACE "^file://" "" DCB_PKG_PATH "\${_u}")
else()
  get_filename_component(DCB_PKG_PATH
    "\${CMAKE_CURRENT_SOURCE_DIR}/../.dart_tool/\${_u}" ABSOLUTE)
endif()
include("\${DCB_PKG_PATH}/native/cmake/dcb_find_package.cmake")
add_subdirectory(\${DCB_ROOT} \${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- Generated wire check ---
set(GEN_WIRE "\${CMAKE_CURRENT_SOURCE_DIR}/generated/wire_dispatch.cpp")
if(NOT EXISTS "\${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run: dcb_gen_tool generate dart_cpp_bridge.yaml")
endif()

# --- Library target ---
option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library($libName
  generated/wire_dispatch.cpp
  api_impl/bridge_api.cpp
)

target_include_directories($libName
  PRIVATE
    \${CMAKE_CURRENT_SOURCE_DIR}
    \${CMAKE_CURRENT_SOURCE_DIR}/api
    \${CMAKE_CURRENT_SOURCE_DIR}/generated
)

target_link_libraries($libName PRIVATE
  \$<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

if(WIN32)
  target_compile_options($libName PRIVATE /utf-8)
endif()
''';

String _hookTemplate(String libName) => '''
import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      return;
    }
    final cmake = Platform.environment['NIX_DCB_CMAKE'] ?? 'cmake';
    final config = switch (input.config.code.targetOS) {
      OS.windows => WindowsConfig(cmake: cmake),
      OS.linux => LinuxConfig(cmake: cmake),
      OS.macOS => MacosConfig(cmake: cmake),
      final os => throw UnsupportedError('$libName does not support: \$os'),
    };
    await DcbCMakeBuilder(
      config: config,
      sourceDir: 'native',
      assetName: 'src/native_gen/dcb_bindings.dart',
      libName: '$libName',
    ).run(input: input, output: output);
  });
}
''';

String _implTemplate(String libName) => '''
#include "bridge_api.h"

#include <thread>
#include <chrono>

// ============================================================
// Example implementation — replace with your own logic.
// ============================================================

std::int32_t add(std::int32_t a, std::int32_t b) {
  return a + b;
}

std::string heavy_compute(std::int32_t input) {
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return "computed: " + std::to_string(input * input);
}

async_simple::coro::Lazy<std::string> fetch_greeting(std::string name) {
  co_return "Hello, " + name + "! (from coroutine)";
}
''';

String _headerTemplate(String libName) => '''
#pragma once

#include "dart_cpp_bridge/annotate.h"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <string>

// ============================================================
// Example API — replace with your own functions.
// ============================================================

/// Sync function → Dart: int add(int a, int b)
BRIDGE_SYNC
std::int32_t add(std::int32_t a, std::int32_t b);

/// Thread-pool async (blocking work) → Dart: Future<String> heavyCompute(int input)
BRIDGE_NORMAL
std::string heavy_compute(std::int32_t input);

/// Coroutine async → Dart: Future<String> fetchGreeting(String name)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> fetch_greeting(std::string name);
''';
