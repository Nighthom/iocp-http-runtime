/// @file multipart.h
/// @brief multipart/form-data 파서

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace iocp::server
{

struct UploadedFile final
{
    std::string filename;
    std::string content_type;
    std::vector<std::byte> data;
    std::size_t size() const noexcept { return data.size(); }
};

class MultipartParser final
{
public:
    explicit MultipartParser(std::string boundary,
        std::size_t max_size = 10 * 1024 * 1024);

    bool Feed(std::string_view data);
    bool IsComplete() const noexcept { return complete_; }
    const std::vector<UploadedFile>& Files() const noexcept { return files_; }

private:
    std::string boundary_;
    std::size_t max_size_;
    std::string buffer_;
    bool complete_{false};
    std::vector<UploadedFile> files_;
};

} // namespace iocp::server
