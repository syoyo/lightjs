#include "fs_compat.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>

#if LIGHTJS_PLATFORM_WINDOWS
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <dirent.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <cstdlib>
  #include <climits>
#endif

namespace lightjs {
namespace fs_compat {

#if LIGHTJS_PLATFORM_WINDOWS

// Windows implementation

bool exists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES;
}

bool isFile(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;
  return !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool isDirectory(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;
  return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool isSymlink(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;
  return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::uint64_t fileSize(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  LARGE_INTEGER size;
  size.HighPart = data.nFileSizeHigh;
  size.LowPart = data.nFileSizeLow;
  return static_cast<std::uint64_t>(size.QuadPart);
}

std::int64_t lastWriteTime(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  // Convert FILETIME to Unix timestamp
  ULARGE_INTEGER ull;
  ull.LowPart = data.ftLastWriteTime.dwLowDateTime;
  ull.HighPart = data.ftLastWriteTime.dwHighDateTime;
  // FILETIME is 100-nanosecond intervals since Jan 1, 1601
  // Convert to Unix epoch (seconds since Jan 1, 1970)
  return static_cast<std::int64_t>((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

FileStatus getStatus(const std::string& path) {
  FileStatus status = {};
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
    status.exists = false;
    return status;
  }
  status.exists = true;
  status.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  status.isFile = !status.isDirectory;
  status.isSymlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

  LARGE_INTEGER size;
  size.HighPart = data.nFileSizeHigh;
  size.LowPart = data.nFileSizeLow;
  status.fileSize = static_cast<std::uint64_t>(size.QuadPart);

  ULARGE_INTEGER ull;
  ull.LowPart = data.ftLastWriteTime.dwLowDateTime;
  ull.HighPart = data.ftLastWriteTime.dwHighDateTime;
  status.lastWriteTime = static_cast<std::int64_t>((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);

  return status;
}

bool createDirectory(const std::string& path) {
  if (isDirectory(path)) return true;
  return CreateDirectoryA(path.c_str(), NULL) != 0;
}

bool createDirectories(const std::string& path) {
  if (path.empty()) return false;
  if (isDirectory(path)) return true;

  // Find parent
  size_t pos = path.find_last_of("/\\");
  if (pos != std::string::npos && pos > 0) {
    std::string parent = path.substr(0, pos);
    if (!parent.empty() && parent != "." && !isDirectory(parent)) {
      if (!createDirectories(parent)) {
        return false;
      }
    }
  }

  return createDirectory(path);
}

bool remove(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;

  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    return RemoveDirectoryA(path.c_str()) != 0;
  } else {
    return DeleteFileA(path.c_str()) != 0;
  }
}

int removeAll(const std::string& path) {
  if (!exists(path)) return 0;

  int count = 0;
  if (isDirectory(path)) {
    auto entries = readDirectory(path);
    for (const auto& entry : entries) {
      int subCount = removeAll(entry.path);
      if (subCount < 0) return -1;
      count += subCount;
    }
  }

  if (remove(path)) {
    return count + 1;
  }
  return -1;
}

bool copyFile(const std::string& src, const std::string& dest, bool overwrite) {
  return CopyFileA(src.c_str(), dest.c_str(), !overwrite) != 0;
}

bool rename(const std::string& oldPath, const std::string& newPath) {
  return MoveFileExA(oldPath.c_str(), newPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::vector<DirectoryEntry> readDirectory(const std::string& path) {
  std::vector<DirectoryEntry> entries;

  std::string searchPath = path;
  if (!searchPath.empty() && searchPath.back() != '/' && searchPath.back() != '\\') {
    searchPath += "\\";
  }
  searchPath += "*";

  WIN32_FIND_DATAA findData;
  HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

  if (hFind == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Cannot open directory: " + path);
  }

  do {
    std::string name = findData.cFileName;
    if (name == "." || name == "..") continue;

    DirectoryEntry entry;
    entry.name = name;
    entry.path = path + "\\" + name;
    entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.isFile = !entry.isDirectory;
    entry.isSymlink = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entries.push_back(entry);
  } while (FindNextFileA(hFind, &findData));

  FindClose(hFind);
  return entries;
}

std::string currentPath() {
  char buffer[MAX_PATH];
  if (GetCurrentDirectoryA(MAX_PATH, buffer) == 0) {
    return ".";
  }
  return std::string(buffer);
}

std::string absolutePath(const std::string& path) {
  char buffer[MAX_PATH];
  if (GetFullPathNameA(path.c_str(), MAX_PATH, buffer, NULL) == 0) {
    return path;
  }
  return std::string(buffer);
}

std::string canonicalPath(const std::string& path) {
  // On Windows, use GetFullPathName for normalization
  return absolutePath(path);
}

#else

// POSIX implementation (Linux, macOS, etc.)

bool exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool isFile(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return false;
  return S_ISREG(st.st_mode);
}

bool isDirectory(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return false;
  return S_ISDIR(st.st_mode);
}

bool isSymlink(const std::string& path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return false;
  return S_ISLNK(st.st_mode);
}

std::uint64_t fileSize(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return static_cast<std::uint64_t>(st.st_size);
}

std::int64_t lastWriteTime(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return static_cast<std::int64_t>(st.st_mtime);
}

FileStatus getStatus(const std::string& path) {
  FileStatus status = {};
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    status.exists = false;
    return status;
  }
  status.exists = true;
  status.isFile = S_ISREG(st.st_mode);
  status.isDirectory = S_ISDIR(st.st_mode);

  // Check symlink with lstat
  struct stat lst;
  if (lstat(path.c_str(), &lst) == 0) {
    status.isSymlink = S_ISLNK(lst.st_mode);
  }

  status.fileSize = static_cast<std::uint64_t>(st.st_size);
  status.lastWriteTime = static_cast<std::int64_t>(st.st_mtime);
  return status;
}

bool createDirectory(const std::string& path) {
  if (isDirectory(path)) return true;
  return mkdir(path.c_str(), 0755) == 0;
}

bool createDirectories(const std::string& path) {
  if (path.empty()) return false;
  if (isDirectory(path)) return true;

  // Find parent
  size_t pos = path.rfind('/');
  if (pos != std::string::npos && pos > 0) {
    std::string parent = path.substr(0, pos);
    if (!parent.empty() && parent != "." && !isDirectory(parent)) {
      if (!createDirectories(parent)) {
        return false;
      }
    }
  }

  return createDirectory(path);
}

bool remove(const std::string& path) {
  if (isDirectory(path)) {
    return rmdir(path.c_str()) == 0;
  } else {
    return unlink(path.c_str()) == 0;
  }
}

int removeAll(const std::string& path) {
  if (!exists(path)) return 0;

  int count = 0;
  if (isDirectory(path)) {
    auto entries = readDirectory(path);
    for (const auto& entry : entries) {
      int subCount = removeAll(entry.path);
      if (subCount < 0) return -1;
      count += subCount;
    }
  }

  if (remove(path)) {
    return count + 1;
  }
  return -1;
}

bool copyFile(const std::string& src, const std::string& dest, bool overwrite) {
  if (!overwrite && exists(dest)) {
    return false;
  }

  int srcFd = open(src.c_str(), O_RDONLY);
  if (srcFd < 0) return false;

  int destFd = open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (destFd < 0) {
    close(srcFd);
    return false;
  }

  char buffer[8192];
  ssize_t bytesRead;
  bool success = true;

  while ((bytesRead = read(srcFd, buffer, sizeof(buffer))) > 0) {
    ssize_t bytesWritten = write(destFd, buffer, bytesRead);
    if (bytesWritten != bytesRead) {
      success = false;
      break;
    }
  }

  if (bytesRead < 0) {
    success = false;
  }

  close(srcFd);
  close(destFd);
  return success;
}

bool rename(const std::string& oldPath, const std::string& newPath) {
  return ::rename(oldPath.c_str(), newPath.c_str()) == 0;
}

std::vector<DirectoryEntry> readDirectory(const std::string& path) {
  std::vector<DirectoryEntry> entries;

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    throw std::runtime_error("Cannot open directory: " + path);
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") continue;

    DirectoryEntry de;
    de.name = name;
    de.path = path + "/" + name;

    // Get file type
    struct stat st;
    if (stat(de.path.c_str(), &st) == 0) {
      de.isFile = S_ISREG(st.st_mode);
      de.isDirectory = S_ISDIR(st.st_mode);
    } else {
      de.isFile = false;
      de.isDirectory = false;
    }

    struct stat lst;
    if (lstat(de.path.c_str(), &lst) == 0) {
      de.isSymlink = S_ISLNK(lst.st_mode);
    } else {
      de.isSymlink = false;
    }

    entries.push_back(de);
  }

  closedir(dir);
  return entries;
}

std::string currentPath() {
  char buffer[PATH_MAX];
  if (getcwd(buffer, PATH_MAX) == nullptr) {
    return ".";
  }
  return std::string(buffer);
}

std::string absolutePath(const std::string& path) {
  if (path.empty()) return currentPath();
  if (path[0] == '/') return path;
  return currentPath() + "/" + path;
}

std::string canonicalPath(const std::string& path) {
  char buffer[PATH_MAX];
  char* result = realpath(path.c_str(), buffer);
  if (result) {
    return std::string(result);
  }
  // If realpath fails (file doesn't exist), return absolute path
  return absolutePath(path);
}

#endif

// Platform-independent implementations

std::string parentPath(const std::string& path) {
  if (path.empty()) return ".";

  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

std::string filename(const std::string& path) {
  if (path.empty()) return "";

  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

std::string extension(const std::string& path) {
  std::string fname = filename(path);
  size_t pos = fname.rfind('.');
  if (pos == std::string::npos || pos == 0) return "";
  return fname.substr(pos);
}

std::string joinPath(const std::string& base, const std::string& component) {
  if (base.empty()) return component;
  if (component.empty()) return base;

  // Check if component is absolute
  if (component[0] == '/' || (component.length() > 1 && component[1] == ':')) {
    return component;
  }

  // Add separator if needed
  char lastChar = base.back();
  if (lastChar == '/' || lastChar == '\\') {
    return base + component;
  }

#if LIGHTJS_PLATFORM_WINDOWS
  return base + "\\" + component;
#else
  return base + "/" + component;
#endif
}

} // namespace fs_compat
} // namespace lightjs
