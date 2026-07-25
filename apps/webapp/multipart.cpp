// multipart/form-data 파서 구현
#include "webapp/multipart.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace iocp::server
{

MultipartParser::MultipartParser(std::string boundary,
    const std::size_t max_size)
    : boundary_("--" + std::move(boundary))
    , max_size_(max_size)
{
}

bool MultipartParser::Feed(const std::string_view data)
{
    buffer_.append(data);
    if (buffer_.size() > max_size_)
        throw std::runtime_error("multipart body exceeds max size");

    const auto end_boundary = boundary_ + "--";
    const auto end_pos = buffer_.find(end_boundary);
    if (end_pos == std::string::npos)
        return false;

    // Find all parts
    std::size_t pos = 0;
    while (pos < buffer_.size())
    {
        const auto part_start = buffer_.find(boundary_, pos);
        if (part_start == std::string::npos) break;
        pos = part_start + boundary_.size();

        // Check for end boundary
        if (buffer_.substr(pos, 2) == "--")
        {
            complete_ = true;
            break;
        }

        // Skip \r\n after boundary
        if (pos + 2 <= buffer_.size() &&
            buffer_[pos] == '\r' && buffer_[pos + 1] == '\n')
            pos += 2;

        // Read headers until \r\n\r\n
        UploadedFile file;
        while (pos < buffer_.size())
        {
            auto line_end = buffer_.find("\r\n", pos);
            if (line_end == std::string::npos) break;
            const auto line = buffer_.substr(pos, line_end - pos);
            pos = line_end + 2;
            if (line.empty()) break; // end of headers

            // Parse Content-Disposition
            if (line.find("Content-Disposition:") == 0)
            {
                const auto fn = line.find("filename=\"");
                if (fn != std::string::npos)
                {
                    const auto fn_start = fn + 10;
                    const auto fn_end = line.find('"', fn_start);
                    if (fn_end != std::string::npos)
                    {
                        file.filename = line.substr(
                            fn_start, fn_end - fn_start);
                    }
                }
            }
            else if (line.find("Content-Type:") == 0)
            {
                file.content_type = line.substr(14);
            }
        }

        // Read body data until next boundary
        const auto next_boundary = buffer_.find(boundary_, pos);
        if (next_boundary == std::string::npos) break;

        // Body ends before next boundary (minus \r\n before boundary)
        auto body_end = next_boundary;
        if (body_end >= 2 &&
            buffer_[body_end - 2] == '\r' &&
            buffer_[body_end - 1] == '\n')
            body_end -= 2;

        const auto body_data = buffer_.substr(pos, body_end - pos);
        file.data.assign(
            reinterpret_cast<const std::byte*>(body_data.data()),
            reinterpret_cast<const std::byte*>(
                body_data.data() + body_data.size()));
        if (!file.filename.empty())
            files_.push_back(std::move(file));

        pos = next_boundary;
    }

    buffer_.clear();
    return true;
}

} // namespace iocp::server
