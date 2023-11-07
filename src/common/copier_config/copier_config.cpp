#include "common/copier_config/copier_config.h"

#include "common/assert.h"
#include "common/exception/copy.h"

namespace kuzu {
namespace common {

const static std::unordered_map<std::string, FileType> fileTypeMap{{".csv", FileType::CSV},
    {".parquet", FileType::PARQUET}, {".npy", FileType::NPY}, {".ttl", FileType::TURTLE}};

FileType FileTypeUtils::getFileTypeFromExtension(const std::string& extension) {
    auto entry = fileTypeMap.find(extension);
    if (entry == fileTypeMap.end()) {
        throw CopyException("Unsupported file type " + extension);
    }
    return entry->second;
}

std::string FileTypeUtils::toString(FileType fileType) {
    switch (fileType) {
    case FileType::UNKNOWN: {
        return "UNKNOWN";
    }
    case FileType::CSV: {
        return "CSV";
    }
    case FileType::PARQUET: {
        return "PARQUET";
    }
    case FileType::NPY: {
        return "NPY";
    }
    case FileType::TURTLE: {
        return "TURTLE";
    }
        // LCOV_EXCL_START
    default: {
        KU_UNREACHABLE;
    }
        // LCOV_EXCL_STOP
    }
}

} // namespace common
} // namespace kuzu
