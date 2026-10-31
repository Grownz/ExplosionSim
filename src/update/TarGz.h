#pragma once

#include <cstddef>
#include <string>

namespace ex::update {

// Extracts a .tar.gz archive into destDir. Absolute paths, parent-directory
// traversal and device entries are rejected. Regular files and directories are
// created with their archived permissions; symlinks and hardlinks are refused
// because release archives never contain them.
bool ExtractTarGz(const std::string& archivePath, const std::string& destDir,
                  std::string& error);

// Extracts an in-memory .tar.gz. Exposed for tests.
bool ExtractTarGzBuffer(const void* data, size_t size,
                        const std::string& destDir, std::string& error);

} // namespace ex::update
