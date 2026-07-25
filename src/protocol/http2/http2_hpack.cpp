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
        // RFC 7541 Appendix B - complete Huffman codes (code, bits)
        static const std::uint32_t kCodes[257][2] = {
            //   0 -  15
            {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},
            {0xfffffe4,28},{0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},
            {0xfffffe8,28},{0xffffea,24},{0x3ffffffc,30},{0xfffffe9,28},
            {0xfffffea,28},{0x3ffffffd,30},{0xfffffeb,28},{0xfffffec,28},
            //  16 -  31
            {0xfffffed,28},{0xfffffee,28},{0xfffffef,28},{0xffffff0,28},
            {0xffffff1,28},{0xffffff2,28},{0x3ffffffe,30},{0xffffff3,28},
            {0xffffff4,28},{0xffffff5,28},{0xffffff6,28},{0xffffff7,28},
            {0xffffff8,28},{0xffffff9,28},{0xffffffa,28},{0xffffffb,28},
            //  32 -  47: ' ' ! " # $ % & ' ( ) * + , - . /
            {0x14,6},{0x3f8,10},{0x3f9,10},{0xffa,12},
            {0x1ff9,13},{0x15,6},{0xf8,8},{0x7fa,11},
            {0x3fa,10},{0x3fb,10},{0xf9,8},{0x7fb,11},
            {0xfa,8},{0x16,6},{0x17,6},{0x18,6},
            //  48 -  63: 0 1 2 3 4 5 6 7 8 9 : ; < = > ?
            {0x00,5},{0x01,5},{0x02,5},{0x19,6},
            {0x1a,6},{0x1b,6},{0x1c,6},{0x1d,6},
            {0x1e,6},{0x1f,6},{0x5c,7},{0xfb,8},
            {0x7ffc,15},{0x20,6},{0xffb,12},{0x3fc,10},
            //  64 -  79: @ A B C D E F G H I J K L M N O
            {0x1ffa,13},{0x21,6},{0x5d,7},{0x5e,7},
            {0x5f,7},{0x60,7},{0x61,7},{0x62,7},
            {0x63,7},{0x64,7},{0x65,7},{0x66,7},
            {0x67,7},{0x68,7},{0x69,7},{0x6a,7},
            //  80 -  95: P Q R S T U V W X Y Z [ \ ] ^ _
            {0x6b,7},{0x6c,7},{0x6d,7},{0x6e,7},
            {0x6f,7},{0x70,7},{0x71,7},{0x72,7},
            {0xfc,8},{0x73,7},{0xfd,8},{0x1ffb,13},
            {0x7fff0,19},{0x1ffc,13},{0x3ffc,14},{0x22,6},
            //  96 - 111: ` a b c d e f g h i j k l m n o
            {0x7ffd,15},{0x03,5},{0x23,6},{0x04,5},
            {0x24,6},{0x05,5},{0x25,6},{0x26,6},
            {0x27,6},{0x06,5},{0x74,7},{0x75,7},
            {0x28,6},{0x29,6},{0x2a,6},{0x07,5},
            // 112 - 127: p q r s t u v w x y z { | } ~
            {0x2b,6},{0x76,7},{0x2c,6},{0x08,5},
            {0x09,5},{0x2d,6},{0x77,7},{0x78,7},
            {0x79,7},{0x7a,7},{0x7b,7},{0x7ffe,15},
            {0x7fc,11},{0x3ffd,14},{0x1ffd,13},{0xffffffc,28},
            // 128 - 143
            {0xfffe6,20},{0x3fffd2,22},{0xfffe7,20},{0xfffe8,20},
            {0x3fffd3,22},{0x3fffd4,22},{0x3fffd5,22},{0x7fffd9,23},
            {0x3fffd6,22},{0x7fffda,23},{0x7fffdb,23},{0x7fffdc,23},
            {0x7fffdd,23},{0x7fffde,23},{0xffffeb,24},{0x7fffdf,23},
            // 144 - 159
            {0xffffec,24},{0xffffed,24},{0x3fffd7,22},{0x7fffe0,23},
            {0xffffee,24},{0x7fffe1,23},{0x7fffe2,23},{0x7fffe3,23},
            {0x7fffe4,23},{0x1fffdc,21},{0x3fffd8,22},{0x7fffe5,23},
            {0x3fffd9,22},{0x7fffe6,23},{0x7fffe7,23},{0xffffef,24},
            // 160 - 175
            {0x3fffda,22},{0x1fffdd,21},{0xfffe9,20},{0x3fffdb,22},
            {0x3fffdc,22},{0x7fffe8,23},{0x7fffe9,23},{0x1fffde,21},
            {0x7fffea,23},{0x3fffdd,22},{0x3fffde,22},{0xfffff0,24},
            {0x1fffdf,21},{0x3fffdf,22},{0x7fffeb,23},{0x7fffec,23},
            // 176 - 191
            {0x1fffe0,21},{0x1fffe1,21},{0x3fffe0,22},{0x1fffe2,21},
            {0x7fffed,23},{0x3fffe1,22},{0x7fffee,23},{0x7fffef,23},
            {0xfffea,20},{0x3fffe2,22},{0x3fffe3,22},{0x3fffe4,22},
            {0x7ffff0,23},{0x3fffe5,22},{0x3fffe6,22},{0x7ffff1,23},
            // 192 - 207
            {0x3ffffe0,26},{0x3ffffe1,26},{0xfffeb,20},{0x7fff1,19},
            {0x3fffe7,22},{0x7ffff2,23},{0x3fffe8,22},{0x1ffffec,25},
            {0x3ffffe2,26},{0x3ffffe3,26},{0x3ffffe4,26},{0x7ffffde,27},
            {0x7ffffdf,27},{0x3ffffe5,26},{0xfffff1,24},{0x1ffffed,25},
            // 208 - 223
            {0x7fff2,19},{0x1fffe3,21},{0x3ffffe6,26},{0x7ffffe0,27},
            {0x7ffffe1,27},{0x3ffffe7,26},{0x7ffffe2,27},{0xfffff2,24},
            {0x1fffe4,21},{0x1fffe5,21},{0x3ffffe8,26},{0x3ffffe9,26},
            {0xfffffd,28},{0x7ffffe3,27},{0x7ffffe4,27},{0x7ffffe5,27},
            // 224 - 239
            {0xfffec,20},{0xfffff3,24},{0xfffed,20},{0x1fffe6,21},
            {0x3fffe9,22},{0x1fffe7,21},{0x1fffe8,21},{0x7ffff3,23},
            {0x3fffea,22},{0x3fffeb,22},{0x1ffffee,25},{0x1ffffef,25},
            {0xfffff4,24},{0xfffff5,24},{0x3ffffea,26},{0x7ffff4,23},
            // 240 - 255
            {0x3ffffeb,26},{0x7ffffe6,27},{0x3ffffec,26},{0x3ffffed,26},
            {0x7ffffe7,27},{0x7ffffe8,27},{0x7ffffe9,27},{0x7ffffea,27},
            {0x7ffffeb,27},{0xffffffe,28},{0x7ffffec,27},{0x7ffffed,27},
            {0x7ffffee,27},{0x7ffffef,27},{0x7fffff0,27},{0x3ffffee,26},
            // 256: EOS
            {0x3fffffff,30},
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

        // RFC 7541 Appendix B - complete Huffman codes (code, bits)
        static const std::uint32_t kCodes[257][2] = {
            //   0 -  15
            {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},
            {0xfffffe4,28},{0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},
            {0xfffffe8,28},{0xffffea,24},{0x3ffffffc,30},{0xfffffe9,28},
            {0xfffffea,28},{0x3ffffffd,30},{0xfffffeb,28},{0xfffffec,28},
            //  16 -  31
            {0xfffffed,28},{0xfffffee,28},{0xfffffef,28},{0xffffff0,28},
            {0xffffff1,28},{0xffffff2,28},{0x3ffffffe,30},{0xffffff3,28},
            {0xffffff4,28},{0xffffff5,28},{0xffffff6,28},{0xffffff7,28},
            {0xffffff8,28},{0xffffff9,28},{0xffffffa,28},{0xffffffb,28},
            //  32 -  47: ' ' ! " # $ % & ' ( ) * + , - . /
            {0x14,6},{0x3f8,10},{0x3f9,10},{0xffa,12},
            {0x1ff9,13},{0x15,6},{0xf8,8},{0x7fa,11},
            {0x3fa,10},{0x3fb,10},{0xf9,8},{0x7fb,11},
            {0xfa,8},{0x16,6},{0x17,6},{0x18,6},
            //  48 -  63: 0 1 2 3 4 5 6 7 8 9 : ; < = > ?
            {0x00,5},{0x01,5},{0x02,5},{0x19,6},
            {0x1a,6},{0x1b,6},{0x1c,6},{0x1d,6},
            {0x1e,6},{0x1f,6},{0x5c,7},{0xfb,8},
            {0x7ffc,15},{0x20,6},{0xffb,12},{0x3fc,10},
            //  64 -  79: @ A B C D E F G H I J K L M N O
            {0x1ffa,13},{0x21,6},{0x5d,7},{0x5e,7},
            {0x5f,7},{0x60,7},{0x61,7},{0x62,7},
            {0x63,7},{0x64,7},{0x65,7},{0x66,7},
            {0x67,7},{0x68,7},{0x69,7},{0x6a,7},
            //  80 -  95: P Q R S T U V W X Y Z [ \ ] ^ _
            {0x6b,7},{0x6c,7},{0x6d,7},{0x6e,7},
            {0x6f,7},{0x70,7},{0x71,7},{0x72,7},
            {0xfc,8},{0x73,7},{0xfd,8},{0x1ffb,13},
            {0x7fff0,19},{0x1ffc,13},{0x3ffc,14},{0x22,6},
            //  96 - 111: ` a b c d e f g h i j k l m n o
            {0x7ffd,15},{0x03,5},{0x23,6},{0x04,5},
            {0x24,6},{0x05,5},{0x25,6},{0x26,6},
            {0x27,6},{0x06,5},{0x74,7},{0x75,7},
            {0x28,6},{0x29,6},{0x2a,6},{0x07,5},
            // 112 - 127: p q r s t u v w x y z { | } ~
            {0x2b,6},{0x76,7},{0x2c,6},{0x08,5},
            {0x09,5},{0x2d,6},{0x77,7},{0x78,7},
            {0x79,7},{0x7a,7},{0x7b,7},{0x7ffe,15},
            {0x7fc,11},{0x3ffd,14},{0x1ffd,13},{0xffffffc,28},
            // 128 - 143
            {0xfffe6,20},{0x3fffd2,22},{0xfffe7,20},{0xfffe8,20},
            {0x3fffd3,22},{0x3fffd4,22},{0x3fffd5,22},{0x7fffd9,23},
            {0x3fffd6,22},{0x7fffda,23},{0x7fffdb,23},{0x7fffdc,23},
            {0x7fffdd,23},{0x7fffde,23},{0xffffeb,24},{0x7fffdf,23},
            // 144 - 159
            {0xffffec,24},{0xffffed,24},{0x3fffd7,22},{0x7fffe0,23},
            {0xffffee,24},{0x7fffe1,23},{0x7fffe2,23},{0x7fffe3,23},
            {0x7fffe4,23},{0x1fffdc,21},{0x3fffd8,22},{0x7fffe5,23},
            {0x3fffd9,22},{0x7fffe6,23},{0x7fffe7,23},{0xffffef,24},
            // 160 - 175
            {0x3fffda,22},{0x1fffdd,21},{0xfffe9,20},{0x3fffdb,22},
            {0x3fffdc,22},{0x7fffe8,23},{0x7fffe9,23},{0x1fffde,21},
            {0x7fffea,23},{0x3fffdd,22},{0x3fffde,22},{0xfffff0,24},
            {0x1fffdf,21},{0x3fffdf,22},{0x7fffeb,23},{0x7fffec,23},
            // 176 - 191
            {0x1fffe0,21},{0x1fffe1,21},{0x3fffe0,22},{0x1fffe2,21},
            {0x7fffed,23},{0x3fffe1,22},{0x7fffee,23},{0x7fffef,23},
            {0xfffea,20},{0x3fffe2,22},{0x3fffe3,22},{0x3fffe4,22},
            {0x7ffff0,23},{0x3fffe5,22},{0x3fffe6,22},{0x7ffff1,23},
            // 192 - 207
            {0x3ffffe0,26},{0x3ffffe1,26},{0xfffeb,20},{0x7fff1,19},
            {0x3fffe7,22},{0x7ffff2,23},{0x3fffe8,22},{0x1ffffec,25},
            {0x3ffffe2,26},{0x3ffffe3,26},{0x3ffffe4,26},{0x7ffffde,27},
            {0x7ffffdf,27},{0x3ffffe5,26},{0xfffff1,24},{0x1ffffed,25},
            // 208 - 223
            {0x7fff2,19},{0x1fffe3,21},{0x3ffffe6,26},{0x7ffffe0,27},
            {0x7ffffe1,27},{0x3ffffe7,26},{0x7ffffe2,27},{0xfffff2,24},
            {0x1fffe4,21},{0x1fffe5,21},{0x3ffffe8,26},{0x3ffffe9,26},
            {0xfffffd,28},{0x7ffffe3,27},{0x7ffffe4,27},{0x7ffffe5,27},
            // 224 - 239
            {0xfffec,20},{0xfffff3,24},{0xfffed,20},{0x1fffe6,21},
            {0x3fffe9,22},{0x1fffe7,21},{0x1fffe8,21},{0x7ffff3,23},
            {0x3fffea,22},{0x3fffeb,22},{0x1ffffee,25},{0x1ffffef,25},
            {0xfffff4,24},{0xfffff5,24},{0x3ffffea,26},{0x7ffff4,23},
            // 240 - 255
            {0x3ffffeb,26},{0x7ffffe6,27},{0x3ffffec,26},{0x3ffffed,26},
            {0x7ffffe7,27},{0x7ffffe8,27},{0x7ffffe9,27},{0x7ffffea,27},
            {0x7ffffeb,27},{0xffffffe,28},{0x7ffffec,27},{0x7ffffed,27},
            {0x7ffffee,27},{0x7ffffef,27},{0x7fffff0,27},{0x3ffffee,26},
            // 256: EOS
            {0x3fffffff,30},
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
                int next = bit == 0
                    ? nodes_[current].left
                    : nodes_[current].right;
                if (next < 0)
                {
                    next = static_cast<int>(nodes_.size());
                    nodes_.push_back({});
                    (bit == 0 ? nodes_[current].left : nodes_[current].right) = next;
                }
                current = next;
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
                EncodeString(result, header.name, true);
            }

            EncodeString(result, header.value, true);

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
