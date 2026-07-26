import 'dart:convert';
import 'dart:io';

/// A downloadable artifact entry with URL and SHA-256 checksum.
class ArtifactEntry {
  final String url;
  final String sha256;

  /// Optional secondary hash (python-build-standalone provides both
  /// `sha256` for the .tar.gz and `archive_sha256` for the inner .tar).
  final String? archiveSha256;

  const ArtifactEntry({
    required this.url,
    required this.sha256,
    this.archiveSha256,
  });

  factory ArtifactEntry.fromJson(Map<String, dynamic> json) {
    return ArtifactEntry(
      url: json['url'] as String,
      sha256: json['sha256'] as String,
      archiveSha256: json['archive_sha256'] as String?,
    );
  }

  /// The file name extracted from the URL (last path segment).
  String get fileName => url.split('/').last;

  /// Content-addressed file name: `<sha256>.<ext>`.
  /// Used as the download cache key in the `downloads/` directory.
  String get contentFileName {
    final name = fileName;
    if (name.endsWith('.tar.gz')) return '$sha256.tar.gz';
    final dotIdx = name.lastIndexOf('.');
    final ext = dotIdx >= 0 ? name.substring(dotIdx) : '';
    return '$sha256$ext';
  }
}

/// Parsed content of `versions.lock`.
///
/// Provides platform-aware accessors for the python-build-standalone
/// distribution and the two vendored wheels (libclang-ng, ruamel.yaml).
class LockFile {
  final String pythonVersion;
  final Map<String, ArtifactEntry> _pythonPlatforms;
  final String libclangVersion;
  final Map<String, ArtifactEntry> _libclangPlatforms;
  final String ruamelVersion;
  final ArtifactEntry _ruamelUniversal;

  LockFile._({
    required this.pythonVersion,
    required Map<String, ArtifactEntry> pythonPlatforms,
    required this.libclangVersion,
    required Map<String, ArtifactEntry> libclangPlatforms,
    required this.ruamelVersion,
    required ArtifactEntry ruamelUniversal,
  })  : _pythonPlatforms = pythonPlatforms,
        _libclangPlatforms = libclangPlatforms,
        _ruamelUniversal = ruamelUniversal;

  /// Parse a `versions.lock` file from disk.
  factory LockFile.parse(File file) {
    if (!file.existsSync()) {
      throw FileSystemException('versions.lock not found', file.path);
    }
    final root =
        jsonDecode(file.readAsStringSync()) as Map<String, dynamic>;

    // --- python ---
    final python = root['python'] as Map<String, dynamic>;
    final pythonPlatforms = <String, ArtifactEntry>{};
    for (final e
        in (python['platforms'] as Map<String, dynamic>).entries) {
      pythonPlatforms[e.key] =
          ArtifactEntry.fromJson(e.value as Map<String, dynamic>);
    }

    // --- libclang_ng (top-level key, underscore) ---
    final libclang = root['libclang_ng'] as Map<String, dynamic>;
    final libclangPlatforms = <String, ArtifactEntry>{};
    for (final e
        in (libclang['platforms'] as Map<String, dynamic>).entries) {
      libclangPlatforms[e.key] =
          ArtifactEntry.fromJson(e.value as Map<String, dynamic>);
    }

    // --- ruamel_yaml (top-level key, underscore; url/sha256 at top level) ---
    final ruamel = root['ruamel_yaml'] as Map<String, dynamic>;
    final ruamelUniversal = ArtifactEntry(
      url: ruamel['url'] as String,
      sha256: ruamel['sha256'] as String,
    );

    return LockFile._(
      pythonVersion: python['version'] as String,
      pythonPlatforms: pythonPlatforms,
      libclangVersion: libclang['version'] as String,
      libclangPlatforms: libclangPlatforms,
      ruamelVersion: ruamel['version'] as String,
      ruamelUniversal: ruamelUniversal,
    );
  }

  /// Get the python-build-standalone artifact for [platformKey]
  /// (e.g. `windows-x86_64`).
  ArtifactEntry pythonFor(String platformKey) {
    final entry = _pythonPlatforms[platformKey];
    if (entry == null) {
      throw StateError(
          'versions.lock has no python entry for platform "$platformKey". '
          'Available: ${_pythonPlatforms.keys.join(', ')}');
    }
    return entry;
  }

  /// Get the libclang-ng wheel artifact for [platformKey].
  ArtifactEntry libclangFor(String platformKey) {
    final entry = _libclangPlatforms[platformKey];
    if (entry == null) {
      throw StateError(
          'versions.lock has no libclang-ng wheel for platform "$platformKey". '
          'Available: ${_libclangPlatforms.keys.join(', ')}');
    }
    return entry;
  }

  /// The ruamel.yaml wheel is platform-independent (py3-none-any).
  ArtifactEntry get ruamel => _ruamelUniversal;
}
