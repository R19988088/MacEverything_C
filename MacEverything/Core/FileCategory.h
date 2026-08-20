#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

enum class FileCategory : uint8_t {
    Files = 0,
    Folders,
    Images,
    Videos,
    Audio,
    Archives,
    Count
};

struct FileCategoryCounts {
    uint64_t files = 0;
    uint64_t folders = 0;
    uint64_t images = 0;
    uint64_t videos = 0;
    uint64_t audio = 0;
    uint64_t archives = 0;
};

namespace me::file_category {

inline constexpr std::array<std::string_view, 13> images = {
    "jpg", "jpeg", "png", "gif", "bmp", "tiff", "tif",
    "webp", "svg", "ico", "heic", "heif", "raw"
};

inline constexpr std::array<std::string_view, 8> videos = {
    "mp4", "avi", "mkv", "mov", "wmv", "flv", "webm", "m4v"
};

inline constexpr std::array<std::string_view, 8> audio = {
    "mp3", "wav", "flac", "aac", "ogg", "m4a", "wma", "alac"
};

inline constexpr std::array<std::string_view, 10> archives = {
    "zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz", "zst", "lz4"
};

inline constexpr std::string_view extensionOf(std::string_view lowerName) {
    const size_t dot = lowerName.rfind('.');
    return dot == std::string_view::npos || dot + 1 == lowerName.size()
        ? std::string_view{}
        : lowerName.substr(dot + 1);
}

template <size_t N>
inline constexpr bool contains(const std::array<std::string_view, N>& values,
                               std::string_view value) {
    for (const auto candidate : values) {
        if (candidate == value) return true;
    }
    return false;
}

inline constexpr bool matches(FileCategory category, uint8_t type,
                              std::string_view lowerName) {
    if (type == 0) return false;
    if (category == FileCategory::Files) return type != 2;
    if (category == FileCategory::Folders) return type == 2;
    if (type == 2) return false;

    const auto ext = extensionOf(lowerName);
    switch (category) {
        case FileCategory::Images: return contains(images, ext);
        case FileCategory::Videos: return contains(videos, ext);
        case FileCategory::Audio: return contains(audio, ext);
        case FileCategory::Archives: return contains(archives, ext);
        default: return false;
    }
}

inline constexpr void accumulate(FileCategoryCounts& counts, uint8_t type,
                                 std::string_view lowerName) {
    if (matches(FileCategory::Files, type, lowerName)) ++counts.files;
    if (matches(FileCategory::Folders, type, lowerName)) ++counts.folders;
    if (matches(FileCategory::Images, type, lowerName)) ++counts.images;
    if (matches(FileCategory::Videos, type, lowerName)) ++counts.videos;
    if (matches(FileCategory::Audio, type, lowerName)) ++counts.audio;
    if (matches(FileCategory::Archives, type, lowerName)) ++counts.archives;
}

} // namespace me::file_category
