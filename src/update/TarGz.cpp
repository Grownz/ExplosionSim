#include "update/TarGz.h"

#include "miniz.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace ex::update {

namespace {

uint64_t ReadOctal(const uint8_t* field, size_t size) {
    if (size > 0 && (field[0] & 0x80) != 0) {
        // GNU base-256 encoding for large values.
        uint64_t value = field[0] & 0x7F;
        for (size_t i = 1; i < size; ++i) {
            value = (value << 8) | field[i];
        }
        return value;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t character = field[i];
        if (character == 0 || character == ' ') {
            break;
        }
        if (character < '0' || character > '7') {
            continue;
        }
        value = value * 8 + uint64_t(character - '0');
    }
    return value;
}

std::string ReadString(const uint8_t* field, size_t size) {
    size_t length = 0;
    while (length < size && field[length] != 0) {
        ++length;
    }
    return std::string(reinterpret_cast<const char*>(field), length);
}

bool IsSafeRelativePath(const std::string& path) {
    if (path.empty() || path[0] == '/') {
        return false;
    }
    const fs::path parsed(path);
    for (const fs::path& part : parsed) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

bool InflateGzip(const uint8_t* data, size_t size,
                 std::vector<uint8_t>& output, std::string& error) {
    if (size < 18 || data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) {
        error = "not a gzip stream";
        return false;
    }
    const uint8_t flags = data[3];
    size_t offset = 10;
    if ((flags & 0x04) != 0) { // FEXTRA
        if (offset + 2 > size) {
            error = "truncated gzip header";
            return false;
        }
        const size_t extra = size_t(data[offset]) | (size_t(data[offset + 1]) << 8);
        offset += 2 + extra;
    }
    if ((flags & 0x08) != 0) { // FNAME
        while (offset < size && data[offset] != 0) {
            ++offset;
        }
        ++offset;
    }
    if ((flags & 0x10) != 0) { // FCOMMENT
        while (offset < size && data[offset] != 0) {
            ++offset;
        }
        ++offset;
    }
    if ((flags & 0x02) != 0) { // FHCRC
        offset += 2;
    }
    if (offset + 8 > size) {
        error = "truncated gzip stream";
        return false;
    }
    const size_t deflateSize = size - offset - 8;
    const uint32_t isize = uint32_t(data[size - 4]) |
                           (uint32_t(data[size - 3]) << 8) |
                           (uint32_t(data[size - 2]) << 16) |
                           (uint32_t(data[size - 1]) << 24);
    if (isize == 0 || isize > (1u << 30)) {
        error = "unreasonable gzip output size";
        return false;
    }

    output.assign(size_t(isize), 0);
    tinfl_decompressor decompressor;
    tinfl_init(&decompressor);
    size_t inputOffset = 0;
    size_t outputOffset = 0;
    for (;;) {
        size_t inBytes = deflateSize - inputOffset;
        size_t outBytes = output.size() - outputOffset;
        const mz_uint32 flagsIn =
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        const tinfl_status status = tinfl_decompress(
            &decompressor, data + offset + inputOffset, &inBytes, output.data(),
            output.data() + outputOffset, &outBytes, flagsIn);
        inputOffset += inBytes;
        outputOffset += outBytes;
        if (status == TINFL_STATUS_DONE) {
            break;
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT) {
            error = "truncated deflate stream";
            return false;
        }
        if (status < 0) {
            error = "invalid deflate stream";
            return false;
        }
        if (outBytes == 0 && status == TINFL_STATUS_HAS_MORE_OUTPUT) {
            error = "gzip output exceeds its declared size";
            return false;
        }
    }
    output.resize(outputOffset);
    return true;
}

} // namespace

bool ExtractTarGzBuffer(const void* data, size_t size,
                        const std::string& destDir, std::string& error) {
    std::vector<uint8_t> tar;
    if (!InflateGzip(static_cast<const uint8_t*>(data), size, tar, error)) {
        return false;
    }

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        error = "cannot create the extraction directory";
        return false;
    }

    size_t offset = 0;
    std::string pendingLongName;
    std::string pendingPaxPath;
    while (offset + 512 <= tar.size()) {
        const uint8_t* header = tar.data() + offset;
        bool empty = true;
        for (size_t i = 0; i < 512; ++i) {
            if (header[i] != 0) {
                empty = false;
                break;
            }
        }
        if (empty) {
            break;
        }

        const uint64_t size = ReadOctal(header + 124, 12);
        const uint64_t mode = ReadOctal(header + 100, 8);
        const char type = char(header[156]);
        std::string name = ReadString(header, 100);
        const std::string prefix = ReadString(header + 345, 155);
        if (!prefix.empty()) {
            name = prefix + "/" + name;
        }
        if (!pendingLongName.empty()) {
            name = pendingLongName;
            pendingLongName.clear();
        }
        if (!pendingPaxPath.empty()) {
            name = pendingPaxPath;
            pendingPaxPath.clear();
        }

        const uint64_t dataOffset = uint64_t(offset) + 512;
        if (dataOffset + size > tar.size()) {
            error = "truncated tar entry";
            return false;
        }
        const uint8_t* payload = tar.data() + dataOffset;

        if (type == 'L') { // GNU long name for the next entry
            pendingLongName.assign(reinterpret_cast<const char*>(payload),
                                   size_t(size));
            while (!pendingLongName.empty() &&
                   pendingLongName.back() == '\0') {
                pendingLongName.pop_back();
            }
            offset = size_t((dataOffset + size + 511) & ~uint64_t(511));
            continue;
        }
        if (type == 'x' || type == 'X') { // pax extended header
            std::string records(reinterpret_cast<const char*>(payload),
                                size_t(size));
            size_t position = 0;
            while (position < records.size()) {
                const size_t space = records.find(' ', position);
                if (space == std::string::npos) {
                    break;
                }
                const size_t length = size_t(std::atoi(
                    records.substr(position, space - position).c_str()));
                if (length == 0 || position + length > records.size()) {
                    break;
                }
                const std::string record =
                    records.substr(space + 1, length - (space + 1 - position) - 1);
                if (record.rfind("path=", 0) == 0) {
                    pendingPaxPath = record.substr(5);
                }
                position += length;
            }
            offset = size_t((dataOffset + size + 511) & ~uint64_t(511));
            continue;
        }

        if (type == '5' || (!name.empty() && name.back() == '/')) { // directory
            if (!IsSafeRelativePath(name)) {
                error = "unsafe archive path: " + name;
                return false;
            }
            fs::create_directories(fs::path(destDir) / name, ec);
            if (ec) {
                error = "cannot create directory: " + name;
                return false;
            }
        } else if (type == '0' || type == '\0') { // regular file
            if (!IsSafeRelativePath(name)) {
                error = "unsafe archive path: " + name;
                return false;
            }
            const fs::path target = fs::path(destDir) / name;
            fs::create_directories(target.parent_path(), ec);
            if (ec) {
                error = "cannot create directory for: " + name;
                return false;
            }
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            if (!file) {
                error = "cannot write: " + name;
                return false;
            }
            file.write(reinterpret_cast<const char*>(payload),
                       std::streamsize(size));
            file.close();
            fs::perms permissions = fs::perms(mode & 0777);
            if ((mode & 0777) == 0) {
                permissions = fs::perms::owner_read | fs::perms::owner_write |
                              fs::perms::group_read | fs::perms::others_read;
            }
            permissions |= fs::perms::owner_read | fs::perms::owner_write;
            fs::permissions(target, permissions, fs::perm_options::replace,
                            ec);
        } else {
            error = "unsupported tar entry type in: " + name;
            return false;
        }

        offset = size_t((dataOffset + size + 511) & ~uint64_t(511));
    }
    return true;
}

bool ExtractTarGz(const std::string& archivePath, const std::string& destDir,
                  std::string& error) {
    std::ifstream file(archivePath, std::ios::binary);
    if (!file) {
        error = "cannot open the archive";
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
    if (data.empty()) {
        error = "the archive is empty";
        return false;
    }
    return ExtractTarGzBuffer(data.data(), data.size(), destDir, error);
}

} // namespace ex::update
