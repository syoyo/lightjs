#pragma once

#include "value.h"
#include <string>
#include <memory>

namespace lightjs {

/**
 * File System API - Node.js-style fs module
 *
 * Provides both synchronous and asynchronous file operations
 */
namespace fs {

// Synchronous file operations

/**
 * Read entire file synchronously
 * @param path File path
 * @param encoding Optional encoding ("utf8", "utf-8", or binary if omitted)
 * @return File contents as string or Uint8Array
 */
Value readFileSync(const std::string& path, const std::string& encoding = "");

/**
 * Write file synchronously
 * @param path File path
 * @param data Data to write (string or Uint8Array)
 */
void writeFileSync(const std::string& path, const Value& data);

/**
 * Append to file synchronously
 * @param path File path
 * @param data Data to append
 */
void appendFileSync(const std::string& path, const Value& data);

/**
 * Check if file/directory exists
 */
bool existsSync(const std::string& path);

/**
 * Delete file
 */
void unlinkSync(const std::string& path);

/**
 * Create directory
 * @param path Directory path
 * @param recursive Create parent directories if true
 */
void mkdirSync(const std::string& path, bool recursive = false);

/**
 * Remove directory
 * @param path Directory path
 * @param recursive Remove recursively if true
 */
void rmdirSync(const std::string& path, bool recursive = false);

/**
 * Read directory contents
 * @return Array of filenames
 */
Value readdirSync(const std::string& path);

/**
 * Get file/directory stats
 * @return Object with size, mtime, isFile(), isDirectory(), etc.
 */
Value statSync(const std::string& path);

/**
 * Copy file
 */
void copyFileSync(const std::string& src, const std::string& dest);

/**
 * Rename/move file
 */
void renameSync(const std::string& oldPath, const std::string& newPath);

// Asynchronous file operations (return Promises)

/**
 * Read file asynchronously
 * @return Promise that resolves to file contents
 */
Value readFile(const std::string& path, const std::string& encoding = "");

/**
 * Write file asynchronously
 * @return Promise that resolves when complete
 */
Value writeFile(const std::string& path, const Value& data);

/**
 * Append to file asynchronously
 * @return Promise that resolves when complete
 */
Value appendFile(const std::string& path, const Value& data);

} // namespace fs

/**
 * Create fs module object with all methods
 */
std::shared_ptr<Object> createFSModule();

} // namespace lightjs
