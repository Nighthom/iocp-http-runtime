// HPACK (RFC 7541) encoder/decoder 구현
#include "protocol/http2/http2_hpack.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace iocp::protocol::http2
{

namespace
{

class HuffmanEncoder final
{
public:
    static std::vector<std::byte> Encode(const std::string& value)
    {
        static const std::uint32_t kCodes[257][2] = {
            {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},
            {0xfffffe4,28},{0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},
            {0xfffffe8,28},{0xfffffea,28},{0x3ffffffc,30},{0xfffffe9,28},
            {0xfffffeb,28},{0x7ffffffd,30},{0xfffffffe,30},{0x3ffffffd,30},
            {0x3ffffffe,30},{0xffffffdd,30},{0x3ffffffb,30},{0x0,5},
            {0x0,5},{0xfffffff0,30},{0xfffffff0,30},{0xfffffff0,30},
            {0xfffffff3,30},{0x3ffffff9,30},{0x0,5},{0x0,5},{0x0,5},
            {0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},
            {0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},
            {0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},
            {0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},
            {0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},
            {0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},
            {0x0,6},{0x0,6},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},
            {0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},
            {0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},
            {0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},
            {0x0,7},{0x0,7},{0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},
            {0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},
            {0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},
            {0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},{0x0,8},
            {0x0,8},{0x0,8},{0x0,10},{0x0,10},{0x0,10},{0x0,10},
            {0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,10},
            {0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,10},
            {0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,10},
            {0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,10},
            {0x0,10},{0x0,10},{0x0,10},{0x0,10},{0x0,11},{0x0,11},
            {0x0,11},{0x0,11},{0x0,11},{0x0,11},{0x0,11},{0x0,11},
            {0x0,11},{0x0,11},{0x0,11},{0x0,11},{0x0,11},{0x0,11},
            {0x0,11},{0x0,11},{0x0,11},{0x0,11},{0x0,11},{0x0,11},
            {0x0,11},{0x0,11},{0x0,11},{0x0,11},{0x0,11},{0x0,11},
            {0x0,12},{0x0,12},{0x0,12},{0x0,12},{0x0,12},{0x0,12},
            {0x0,12},{0x0,12},{0x0,12},{0x0,12},{0x0,12},{0x0,12},
            {0x0,12},{0x0,12},{0x0,12},{0x0,12},{0x0,12},{0x0,12},
            {0x0,12},{0x0,12},{0x0,12},{0x0,12},{0x0,12},{0x0,12},
            {0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},
            {0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},
            {0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},
            {0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},
            {0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},{0x0,13},
            {0x1fffffa,28},
        };

        std::uint64_t current = 0;
        int bits_filled = 0;
        std::vector<std::byte> result;

        for (const unsigned char character : value)
        {
            const auto code = kCodes[character][0];
            const auto bits = kCodes[character][1];
            if (bits == 0) continue;

            current = (current << bits) | code;
            bits_filled += static_cast<int>(bits);

            while (bits_filled >= 8)
            {
                bits_filled -= 8;
                result.push_back(
                    static_cast<std::byte>(
                        (current >> bits_filled) & 0xff));
            }
        }

        if (bits_filled > 0)
        {
            result.push_back(static_cast<std::byte>(
                (current << (8 - bits_filled)) |
                ((1 << (8 - bits_filled)) - 1)));
        }

        return result;
    }
};

class HuffmanDecoder final
{
public:
    struct Node final
    {
        int value{-1};
        int left{-1};
        int right{-1};
    };

    static HuffmanDecoder& Instance()
    {
        static HuffmanDecoder decoder;
        return decoder;
    }

    bool Decode(
        const std::byte* data,
        const std::size_t byte_length,
        std::string& output) const
    {
        std::string result;
        int node = 0;
        std::uint8_t buffer = 0;
        int bits_remaining = 0;

        for (std::size_t i = 0; i < byte_length; ++i)
        {
            buffer = static_cast<std::uint8_t>(data[i]);
            bits_remaining = 8;

            while (bits_remaining > 0)
            {
                --bits_remaining;
                const int bit = (buffer >> bits_remaining) & 1;
                const int next = bit == 0
                    ? nodes_[node].left
                    : nodes_[node].right;

                if (next < 0) return false;
                node = next;
                if (nodes_[node].value >= 0)
                {
                    if (nodes_[node].value == 256)
                    {
                        return i + 1 >= byte_length;
                    }
                    result.push_back(
                        static_cast<char>(nodes_[node].value));
                    node = 0;
                }
            }
        }

        output = std::move(result);
        return true;
    }

private:
    HuffmanDecoder() { BuildTree(); }

    void BuildTree()
    {
        nodes_.clear();
        nodes_.push_back({});

        static const std::uint32_t kCodes[257][2] = {
            {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},
            {0xfffffe4,28},{0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},
            {0xfffffe8,28},{0xfffffea,28},{0x3ffffffc,30},{0xfffffe9,28},
            {0xfffffeb,28},{0x7ffffffd,30},{0xfffffffe,30},{0x3ffffffd,30},
            {0x3ffffffe,30},{0xffffffdd,30},{0x3ffffffb,30},{0x0,5},
            {0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},
            {0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},
            {0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},{0x0,5},
            {0x0,5},{0x0,5},{0x0,5},{0x0,6},{0x0,6},{0x0,6},{0x0,6},
            {0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},
            {0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},
            {0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,6},
            {0x0,6},{0x0,6},{0x0,6},{0x0,6},{0x0,7},{0x0,7},{0x0,7},
            {0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},
            {0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},
            {0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,7},
            {0x0,7},{0x0,7},{0x0,7},{0x0,7},{0x0,8},{0x0,8},{0x0,8},
        };

        for (int symbol = 0; symbol < 257; ++symbol)
        {
            const auto code = kCodes[symbol][0];
            const auto bits = kCodes[symbol][1];
            if (bits == 0) continue;

            int current = 0;
            for (int b = static_cast<int>(bits) - 1; b >= 0; --b)
            {
                const int bit = (static_cast<int>(code) >> b) & 1;
                int& child = bit == 0
                    ? nodes_[current].left
                    : nodes_[current].right;
                if (child < 0)
                {
                    child = static_cast<int>(nodes_.size());
                    nodes_.push_back({});
                }
                current = child;
            }
            nodes_[current].value = symbol;
        }
    }

    std::vector<Node> nodes_;
};

} // namespace

std::vector<HpackCodec::TableEntry> HpackCodec::BuildStaticTable()
{
    std::vector<TableEntry> table;
    table.reserve(62);
    table.push_back({});

    table.push_back({":authority", ""});
    table.push_back({":method", "GET"});
    table.push_back({":method", "POST"});
    table.push_back({":path", "/"});
    table.push_back({":path", "/index.html"});
    table.push_back({":scheme", "http"});
    table.push_back({":scheme", "https"});
    table.push_back({":status", "200"});
    table.push_back({":status", "204"});
    table.push_back({":status", "206"});
    table.push_back({":status", "304"});
    table.push_back({":status", "400"});
    table.push_back({":status", "404"});
    table.push_back({":status", "500"});
    table.push_back({"accept-charset", ""});
    table.push_back({"accept-encoding", "gzip, deflate"});
    table.push_back({"accept-language", ""});
    table.push_back({"accept-ranges", ""});
    table.push_back({"accept", ""});
    table.push_back({"access-control-allow-origin", ""});
    table.push_back({"age", ""});
    table.push_back({"allow", ""});
    table.push_back({"authorization", ""});
    table.push_back({"cache-control", ""});
    table.push_back({"content-disposition", ""});
    table.push_back({"content-encoding", ""});
    table.push_back({"content-language", ""});
    table.push_back({"content-length", ""});
    table.push_back({"content-location", ""});
    table.push_back({"content-range", ""});
    table.push_back({"content-type", ""});
    table.push_back({"cookie", ""});
    table.push_back({"date", ""});
    table.push_back({"etag", ""});
    table.push_back({"expect", ""});
    table.push_back({"expires", ""});
    table.push_back({"from", ""});
    table.push_back({"host", ""});
    table.push_back({"if-match", ""});
    table.push_back({"if-modified-since", ""});
    table.push_back({"if-none-match", ""});
    table.push_back({"if-range", ""});
    table.push_back({"if-unmodified-since", ""});
    table.push_back({"last-modified", ""});
    table.push_back({"link", ""});
    table.push_back({"location", ""});
    table.push_back({"max-forwards", ""});
    table.push_back({"proxy-authenticate", ""});
    table.push_back({"proxy-authorization", ""});
    table.push_back({"range", ""});
    table.push_back({"referer", ""});
    table.push_back({"refresh", ""});
    table.push_back({"retry-after", ""});
    table.push_back({"server", ""});
    table.push_back({"set-cookie", ""});
    table.push_back({"strict-transport-security", ""});
    table.push_back({"transfer-encoding", ""});
    table.push_back({"user-agent", ""});
    table.push_back({"vary", ""});
    table.push_back({"via", ""});
    table.push_back({"www-authenticate", ""});

    return table;
}

HpackCodec::HpackCodec(const HpackOptions options)
    : static_table_(BuildStaticTable())
    , options_(options)
{
}

static std::uint32_t ReadDecodedInteger(
    const std::byte* data,
    std::size_t& offset,
    const std::size_t size,
    const std::uint8_t prefix_bits)
{
    if (offset >= size)
    {
        throw std::runtime_error("HPACK decode: unexpected end of data");
    }

    const std::uint8_t mask = (1 << prefix_bits) - 1;
    std::uint32_t value =
        static_cast<std::uint8_t>(data[offset]) & mask;
    ++offset;

    if (value < static_cast<std::uint32_t>(mask))
    {
        return value;
    }

    std::uint32_t m = 0;
    for (;;)
    {
        if (offset >= size)
        {
            throw std::runtime_error(
                "HPACK decode: unexpected end of integer continuation");
        }
        const std::uint8_t byte =
            static_cast<std::uint8_t>(data[offset]);
        ++offset;
        value += (byte & 0x7f) << m;
        m += 7;
        if ((byte & 0x80) == 0) break;
        if (m > 28)
        {
            throw std::runtime_error("HPACK decode: integer overflow");
        }
    }
    return value;
}

std::uint32_t HpackCodec::DecodeInteger(
    const std::byte* data,
    std::size_t& offset,
    const std::size_t size,
    const std::uint8_t prefix_bits) const
{
    return ReadDecodedInteger(data, offset, size, prefix_bits);
}

std::string HpackCodec::DecodeString(
    const std::byte* data,
    std::size_t& offset,
    const std::size_t size) const
{
    if (offset >= size)
    {
        throw std::runtime_error("HPACK decode: expected string");
    }

    const bool huffman =
        (static_cast<std::uint8_t>(data[offset]) & 0x80) != 0;
    const std::uint32_t length =
        ReadDecodedInteger(data, offset, size, 7);

    if (offset + length > size)
    {
        throw std::runtime_error(
            "HPACK decode: string length exceeds data");
    }

    std::string result;
    if (huffman)
    {
        if (!HuffmanDecoder::Instance().Decode(
                data + offset, length, result))
        {
            throw std::runtime_error(
                "HPACK decode: invalid Huffman string");
        }
    }
    else
    {
        result.assign(
            reinterpret_cast<const char*>(data + offset), length);
    }

    offset += length;
    return result;
}

std::vector<http::HttpHeader> HpackCodec::Decode(
    const std::byte* data,
    const std::size_t size)
{
    std::vector<http::HttpHeader> headers;
    std::size_t offset = 0;
    std::size_t header_list_size = 0;

    while (offset < size)
    {
        const std::uint8_t first_byte =
            static_cast<std::uint8_t>(data[offset]);

        if (first_byte & 0x80)
        {
            const std::uint32_t index =
                ReadDecodedInteger(data, offset, size, 7);
            if (index == 0)
            {
                throw std::runtime_error(
                    "HPACK: invalid indexed header index 0");
            }

            const auto& entry = [&]() -> const TableEntry& {
                if (index <= static_table_.size())
                {
                    return static_table_[index];
                }
                return dynamic_table_[index - static_table_.size() - 1];
            }();

            headers.push_back({entry.name, entry.value});
            header_list_size +=
                entry.name.size() + entry.value.size() + 32;
        }
        else if ((first_byte & 0xc0) == 0x40)
        {
            http::HttpHeader header;
            const std::uint32_t name_index =
                ReadDecodedInteger(data, offset, size, 6);

            if (name_index > 0)
            {
                const auto& entry = [&]() -> const TableEntry& {
                    if (name_index <= static_table_.size())
                    {
                        return static_table_[name_index];
                    }
                    return dynamic_table_[
                        name_index - static_table_.size() - 1];
                }();
                header.name = entry.name;
            }
            else
            {
                header.name = DecodeString(data, offset, size);
            }

            header.value = DecodeString(data, offset, size);
            headers.push_back(header);
            header_list_size +=
                header.name.size() + header.value.size() + 32;

            dynamic_table_.insert(
                dynamic_table_.begin(),
                {header.name, header.value});
            dynamic_table_size_ +=
                header.name.size() + header.value.size() + 32;
        }
        else if ((first_byte & 0xf0) == 0x00)
        {
            http::HttpHeader header;
            const std::uint32_t name_index =
                ReadDecodedInteger(data, offset, size, 4);

            if (name_index > 0)
            {
                const auto& entry = [&]() -> const TableEntry& {
                    if (name_index <= static_table_.size())
                    {
                        return static_table_[name_index];
                    }
                    return dynamic_table_[
                        name_index - static_table_.size() - 1];
                }();
                header.name = entry.name;
            }
            else
            {
                header.name = DecodeString(data, offset, size);
            }

            header.value = DecodeString(data, offset, size);
            headers.push_back(header);
            header_list_size +=
                header.name.size() + header.value.size() + 32;
        }
        else if ((first_byte & 0xe0) == 0x20)
        {
            const std::uint32_t new_size =
                ReadDecodedInteger(data, offset, size, 5);
            SetDynamicTableSize(new_size);
        }
        else
        {
            throw std::runtime_error(
                "HPACK: unknown header representation");
        }

        if (header_list_size > options_.maximum_header_list_size)
        {
            throw std::runtime_error(
                "HPACK: header list size exceeds limit");
        }
    }

    return headers;
}

std::vector<std::byte> HpackCodec::Encode(
    const std::vector<http::HttpHeader>& headers)
{
    std::vector<std::byte> result;

    for (const auto& header : headers)
    {
        int static_index = -1;
        for (std::size_t i = 1; i < static_table_.size(); ++i)
        {
            if (static_table_[i].name == header.name &&
                static_table_[i].value == header.value)
            {
                static_index = static_cast<int>(i);
                break;
            }
        }

        if (static_index > 0)
        {
            result.push_back(static_cast<std::byte>(0x80));
            EncodeInteger(result, static_index, 7);
        }
        else
        {
            int name_index = -1;
            for (std::size_t i = 1; i < static_table_.size(); ++i)
            {
                if (static_table_[i].name == header.name)
                {
                    name_index = static_cast<int>(i);
                    break;
                }
            }

            if (name_index > 0)
            {
                result.push_back(static_cast<std::byte>(0x40));
                EncodeInteger(result, name_index, 6);
            }
            else
            {
                result.push_back(static_cast<std::byte>(0x40));
                EncodeInteger(result, 0, 6);
                EncodeString(result, header.name, false);
            }

            EncodeString(result, header.value, false);

            dynamic_table_.insert(
                dynamic_table_.begin(),
                {header.name, header.value});
            dynamic_table_size_ +=
                header.name.size() + header.value.size() + 32;
        }
    }

    return result;
}

void HpackCodec::SetDynamicTableSize(
    const std::size_t new_size)
{
    dynamic_table_size_ = 0;

    while (!dynamic_table_.empty())
    {
        const auto& entry = dynamic_table_.back();
        dynamic_table_size_ -=
            entry.name.size() + entry.value.size() + 32;
        dynamic_table_.pop_back();
    }

    if (new_size == 0)
    {
        dynamic_table_.clear();
        dynamic_table_size_ = 0;
    }
}

void HpackCodec::EncodeInteger(
    std::vector<std::byte>& output,
    const std::uint32_t value,
    const std::uint8_t prefix_bits) const
{
    const std::uint8_t mask = (1 << prefix_bits) - 1;

    if (value < mask)
    {
        output.back() =
            static_cast<std::byte>(
                static_cast<std::uint8_t>(output.back()) | value);
        return;
    }

    output.back() =
        static_cast<std::byte>(
            static_cast<std::uint8_t>(output.back()) | mask);

    std::uint32_t remaining = value - mask;
    while (remaining >= 128)
    {
        output.push_back(
            static_cast<std::byte>((remaining & 0x7f) | 0x80));
        remaining >>= 7;
    }
    output.push_back(static_cast<std::byte>(remaining & 0x7f));
}

void HpackCodec::EncodeString(
    std::vector<std::byte>& output,
    const std::string& value,
    const bool use_huffman) const
{
    if (use_huffman)
    {
        const auto encoded = HuffmanEncoder::Encode(value);
        output.push_back(static_cast<std::byte>(0x80));
        EncodeInteger(output,
            static_cast<std::uint32_t>(encoded.size()), 7);
        output.insert(output.end(), encoded.begin(), encoded.end());
    }
    else
    {
        output.push_back(static_cast<std::byte>(0x00));
        EncodeInteger(output,
            static_cast<std::uint32_t>(value.size()), 7);
        for (const char character : value)
        {
            output.push_back(static_cast<std::byte>(character));
        }
    }
}

} // namespace iocp::protocol::http2
