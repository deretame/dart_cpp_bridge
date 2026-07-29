import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:yaml/yaml.dart';

import 'commands.dart';
import 'package_root.dart';

/// `dcb_gen init --name <lib_name>`
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

  if (libName == null || libName.isEmpty) {
    stderr.writeln('error: --name is required.');
    stderr.writeln('Usage: dcb_gen init --name <library_name>');
    return 1;
  }

  // Validate name (CMake target name: alphanumeric + underscore).
  if (!RegExp(r'^[a-zA-Z_][a-zA-Z0-9_]*$').hasMatch(libName)) {
    stderr.writeln(
        'error: invalid library name "$libName". '
        'Use only letters, digits, and underscores (must start with a letter).');
    return 1;
  }

  final cwd = Directory.current.path;

  // Check for existing files that would be overwritten.
  final conflicts = <String>[];
  for (final f in ['dart_cpp_bridge.yaml', 'CMakeLists.txt']) {
    if (File(p.join(cwd, f)).existsSync()) conflicts.add(f);
  }
  if (Directory(p.join(cwd, 'native', 'api')).existsSync()) {
    conflicts.add('native/api/');
  }
  if (conflicts.isNotEmpty) {
    stderr.writeln('error: the following files already exist in $cwd:');
    for (final c in conflicts) {
      stderr.writeln('  - $c');
    }
    stderr.writeln('Remove them or run init in an empty directory.');
    return 1;
  }

  log.info('Initializing dart_cpp_bridge project "$libName" in $cwd ...');

  // Resolve dart_cpp_bridge native include path for the yaml config.
  final nativeInclude = await _resolveNativeInclude(cwd, log);

  // --- Generate files ---
  _writeFile(
      p.join(cwd, 'dart_cpp_bridge.yaml'), _yamlTemplate(libName, nativeInclude));
  log.info('  created dart_cpp_bridge.yaml');

  _writeFile(p.join(cwd, 'CMakeLists.txt'), _cmakeTemplate(libName));
  log.info('  created CMakeLists.txt');

  final apiDir = Directory(p.join(cwd, 'native', 'api'));
  apiDir.createSync(recursive: true);
  _writeFile(p.join(apiDir.path, 'bridge_api.h'), _headerTemplate(libName));
  log.info('  created native/api/bridge_api.h');

  // --- Run generate ---
  log.info('Running initial codegen ...');
  final configPath = p.join(cwd, 'dart_cpp_bridge.yaml');
  final exitCode = await runGenerate(
    [configPath],
    force: force,
    quiet: quiet,
  );
  if (exitCode != 0) return exitCode;

  log.info('Done! Next steps:');
  log.info('  1. Add dart_cpp_bridge to your pubspec.yaml dependencies');
  log.info('  2. Run "dart pub get"');
  log.info('  3. Write a hook/build.dart to invoke CMake');
  log.info('  4. Implement your functions in native/api/bridge_api.h');
  log.info('  5. Run "dcb_gen generate dart_cpp_bridge.yaml" after changes');
  return 0;
}

/// Resolves the dart_cpp_bridge native/include directory.
///
/// Strategy:
///   1. From .dart_tool/package_config.json (if user already ran pub get)
///   2. From the tool's own location (monorepo: dcb_gen_tool/../dart/native/include)
Future<String?> _resolveNativeInclude(String cwd, CliLogger log) async {
  // Strategy 1: package_config.json
  final pkgConfigFile = File(p.join(cwd, '.dart_tool', 'package_config.json'));
  if (pkgConfigFile.existsSync()) {
    try {
      final content = pkgConfigFile.readAsStringSync();
      final yaml = loadYaml(content) as YamlMap;
      final packages = yaml['packages'] as YamlList?;
      if (packages != null) {
        for (final pkg in packages) {
          final map = pkg as YamlMap;
          if (map['name'] != 'dart_cpp_bridge') continue;
          final rootUri = map['rootUri'] as String;
          String pkgPath;
          if (rootUri.startsWith('file://')) {
            pkgPath = Platform.isWindows
                ? rootUri.substring('file:///'.length)
                : rootUri.substring('file://'.length);
          } else {
            pkgPath = p.normalize(
                p.join(p.dirname(pkgConfigFile.path), rootUri));
          }
          final inc = p.join(pkgPath, 'native', 'include');
          if (Directory(inc).existsSync()) return inc;
        }
      }
    } catch (_) {}
  }

  // Strategy 2: monorepo sibling (dcb_gen_tool/../dart/native/include)
  try {
    final toolRoot = await resolvePackageRoot();
    final inc = p.normalize(p.join(toolRoot, '..', 'dart', 'native', 'include'));
    if (Directory(inc).existsSync()) return inc;
  } catch (_) {}

  log.warn('Could not resolve dart_cpp_bridge native/include path.\n'
      '  Run "dart pub get" after adding dart_cpp_bridge to pubspec.yaml,\n'
      '  then re-run "dcb_gen generate dart_cpp_bridge.yaml".');
  return null;
}

void _writeFile(String path, String content) {
  File(path).writeAsStringSync(content);
}

// ===========================================================================
// Templates
// ===========================================================================

String _yamlTemplate(String libName, String? nativeInclude) {
  final includePaths = <String>[
    'native',
    'native/api',
    if (nativeInclude != null) nativeInclude.replaceAll('\\', '/'),
  ];
  final includeYaml =
      includePaths.map((e) => '  - $e').join('\n');
  return '''
# dart_cpp_bridge codegen config for $libName
dart_package: $libName
cpp_root: native/

scan:
  - native/api/

include_paths:
$includeYaml

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

# --- Locate dart_cpp_bridge via package_config.json ---
set(PACKAGE_CONFIG "\${CMAKE_CURRENT_SOURCE_DIR}/.dart_tool/package_config.json")
if(NOT EXISTS "\${PACKAGE_CONFIG}")
  message(FATAL_ERROR "Missing .dart_tool/package_config.json. Run 'dart pub get' first.")
endif()

file(READ "\${PACKAGE_CONFIG}" _pkg_json)

set(_dcb_pkg_index -1)
string(JSON _pkg_count LENGTH "\${_pkg_json}" "packages")
math(EXPR _pkg_last "\${_pkg_count} - 1")
foreach(i RANGE \${_pkg_last})
  string(JSON _name GET "\${_pkg_json}" "packages" \${i} "name")
  if(_name STREQUAL "dart_cpp_bridge")
    set(_dcb_pkg_index \${i})
    break()
  endif()
endforeach()

if(_dcb_pkg_index EQUAL -1)
  message(FATAL_ERROR "dart_cpp_bridge not found in package_config.json")
endif()

string(JSON _root_uri GET "\${_pkg_json}" "packages" \${_dcb_pkg_index} "rootUri")

if(_root_uri MATCHES "^file://")
  if(WIN32)
    string(REGEX REPLACE "^file:///" "" _dcb_pkg_path "\${_root_uri}")
  else()
    string(REGEX REPLACE "^file://" "" _dcb_pkg_path "\${_root_uri}")
  endif()
else()
  get_filename_component(_dcb_pkg_path
    "\${CMAKE_CURRENT_SOURCE_DIR}/.dart_tool/\${_root_uri}" ABSOLUTE)
endif()

message(STATUS "dart_cpp_bridge package: \${_dcb_pkg_path}")

# --- Base runtime (asio / async-simple propagated transitively) ---
set(DCB_ROOT "\${_dcb_pkg_path}/native")
add_subdirectory(\${DCB_ROOT} \${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- Generated wire check ---
set(GEN_WIRE "\${CMAKE_CURRENT_SOURCE_DIR}/native/generated/wire_dispatch.cpp")
if(NOT EXISTS "\${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run: dcb_gen generate dart_cpp_bridge.yaml")
endif()

# --- Library target ---
option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library($libName
  native/generated/wire_dispatch.cpp
  # Add your implementation files here:
  # native/api_impl/bridge_api.cpp
)

target_include_directories($libName
  PRIVATE
    \${CMAKE_CURRENT_SOURCE_DIR}/native
    \${CMAKE_CURRENT_SOURCE_DIR}/native/api
    \${CMAKE_CURRENT_SOURCE_DIR}/native/generated
)

target_link_libraries($libName PRIVATE
  \$<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

if(WIN32)
  target_compile_options($libName PRIVATE /utf-8)
endif()
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
