/// @file http2_hpack.h
/// @brief HTTP/2 HPACK (RFC 7541) header compression encoder/decoder

#pragma once

#include "protocol/http/http_message.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iocp::protocol::http2
{

struct HpackOptions final
{
    std::size_t maximum_dynamic_table_size{4096};
    std::size_t maximum_header_list_size{16384};
};

/// @brief HPACK integer encoding/decoding 및 header field compression을 제공한다.
///
/// static table (RFC 7541 Appendix A) 기반의 decoding과 기본 Huffman codec을
/// 포함한다. dynamic table은 최대 크기 제한 내에서 관리한다.
class HpackCodec final
{
public:
    explicit HpackCodec(HpackOptions options = {});

    /// @brief HPACK 요청 헤더 block을 decode한다.
    /// @throws std::runtime_error decompression error 발생 시.
    std::vector<http::HttpHeader> Decode(
        const std::byte* data,
        std::size_t size);

    /// @brief HTTP header 목록을 HPACK으로 encode한다.
    std::vector<std::byte> Encode(
        const std::vector<http::HttpHeader>& headers);

    /// @brief dynamic table 크기를 변경한다.
    void SetDynamicTableSize(std::size_t new_size);

private:
    struct TableEntry final
    {
        std::string name;
        std::string value;
    };

    static std::vector<TableEntry> BuildStaticTable();

    const TableEntry& Lookup(std::uint32_t index) const;
    void AddDynamicEntry(
        const std::string& name,
        const std::string& value);
    void EvictDynamicTable();

    std::uint32_t DecodeInteger(
        const std::byte* data,
        std::size_t& offset,
        std::size_t size,
        std::uint8_t prefix_bits) const;

    std::string DecodeString(
        const std::byte* data,
        std::size_t& offset,
        std::size_t size) const;

    static bool DecodeHuffmanString(
        const std::byte* data,
        std::size_t byte_length,
        std::string& output);

    void EncodeInteger(
        std::vector<std::byte>& output,
        std::uint32_t value,
        std::uint8_t prefix_bits) const;

    void EncodeString(
        std::vector<std::byte>& output,
        const std::string& value,
        bool use_huffman = true) const;

    static std::vector<std::byte> EncodeHuffmanString(
        const std::string& value);

    std::vector<TableEntry> static_table_;
    std::vector<TableEntry> dynamic_table_;
    std::size_t dynamic_table_size_{};
    std::size_t current_dynamic_table_limit_{};
    std::optional<std::size_t> pending_table_size_update_;
    HpackOptions options_;
};

} // namespace iocp::protocol::http2
