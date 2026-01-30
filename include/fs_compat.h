#pragma once

/**
 * LightJS Filesystem Compatibility Layer
 *
 * This header provides a platform-independent filesystem API that works
 * with both C++17 and C++20. In C++20 mode with std::filesystem available,
 * these functions are thin wrappers. In C++17 mode or when std::filesystem
 * is unavailable, they use POSIX/Win32 APIs directly.
 */

#include "compat.h"
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace lightjs {
namespace fs_compat {

/**
 * Directory entry information
 */
struct DirectoryEntry {
  std::string name;        // Filename only (not full path)
  std::string path;        // Full path
  bool isFile;
  bool isDirectory;
  bool isSymlink;
};

/**
 * File/directory status information
 */
struct FileStatus {
  bool exists;
  bool isFile;
  bool isDirectory;
  bool isSymlink;
  std::uint64_t fileSize;
  std::int64_t lastWriteTime;  // Unix timestamp in seconds
};

/**
 * Check if a file or directory exists
 */
bool exists(const std::string& path);

/**
 * Check if path is a regular file
 */
bool isFile(const std::string& path);

/**
 * Check if path is a directory
 */
bool isDirectory(const std::string& path);

/**
 * Check if path is a symbolic link
 */
bool isSymlink(const std::string& path);

/**
 * Get file size in bytes
 * Returns 0 if file doesn't exist or is a directory
 */
std::uint64_t fileSize(const std::string& path);

/**
 * Get last modification time as Unix timestamp (seconds since epoch)
 */
std::int64_t lastWriteTime(const std::string& path);

/**
 * Get complete file status in one call (more efficient for multiple checks)
 */
FileStatus getStatus(const std::string& path);

/**
 * Create a single directory
 * Returns true on success or if directory already exists
 * Returns false if parent directory doesn't exist
 */
bool createDirectory(const std::string& path);

/**
 * Create directory and all parent directories as needed
 * Returns true on success
 */
bool createDirectories(const std::string& path);

/**
 * Remove a file or empty directory
 * Returns true on success
 */
bool remove(const std::string& path);

/**
 * Remove a directory and all its contents recursively
 * Returns the number of items removed, or -1 on error
 */
int removeAll(const std::string& path);

/**
 * Copy a file
 * Returns true on success
 */
bool copyFile(const std::string& src, const std::string& dest, bool overwrite = true);

/**
 * Rename/move a file or directory
 * Returns true on success
 */
bool rename(const std::string& oldPath, const std::string& newPath);

/**
 * Read directory contents
 * Returns vector of DirectoryEntry structures
 * Throws std::runtime_error on failure
 */
std::vector<DirectoryEntry> readDirectory(const std::string& path);

/**
 * Get the current working directory
 */
std::string currentPath();

/**
 * Get absolute path
 */
std::string absolutePath(const std::string& path);

/**
 * Get canonical path (resolves symlinks and normalizes)
 * May throw if path doesn't exist
 */
std::string canonicalPath(const std::string& path);

/**
 * Get parent directory path
 */
std::string parentPath(const std::string& path);

/**
 * Get filename from path
 */
std::string filename(const std::string& path);

/**
 * Get file extension (including the dot)
 */
std::string extension(const std::string& path);

/**
 * Join two path components
 */
std::string joinPath(const std::string& base, const std::string& component);

} // namespace fs_compat
} // namespace lightjs
