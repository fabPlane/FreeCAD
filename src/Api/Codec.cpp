// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Codec.h"

#include <array>
#include <stdexcept>

namespace Api
{

namespace
{

constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// JSON text has no bytes: {"$bytes": base64} stands in for them in both directions.
Json binaryToTagged(const Json& value)
{
    if (value.is_binary()) {
        const auto& bin = value.get_binary();
        return Json {{"$bytes", base64Encode(bin.data(), bin.size())}};
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[key] = binaryToTagged(item);
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(binaryToTagged(item));
        }
        return out;
    }
    return value;
}

void taggedToBinary(Json& value)
{
    if (value.is_object()) {
        if (value.size() == 1 && value.contains("$bytes") && value["$bytes"].is_string()) {
            Bytes raw = base64Decode(value["$bytes"].get<std::string>());
            value = Json::binary(std::move(raw));
            return;
        }
        for (auto& [key, item] : value.items()) {
            (void)key;
            taggedToBinary(item);
        }
    }
    else if (value.is_array()) {
        for (auto& item : value) {
            taggedToBinary(item);
        }
    }
}

}  // namespace

Encoding detectEncoding(const std::uint8_t* data, std::size_t size)
{
    // Skip the whitespace a hand-written JSON message may start with.
    for (std::size_t i = 0; i < size; ++i) {
        const auto c = data[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        return c == '{' ? Encoding::Json : Encoding::Cbor;
    }
    return Encoding::Json;
}

Json decode(const std::uint8_t* data, std::size_t size, Encoding encoding)
{
    if (encoding == Encoding::Cbor) {
        return Json::from_cbor(data, data + size);
    }
    Json value = Json::parse(data, data + size);
    taggedToBinary(value);
    return value;
}

Bytes encode(const Json& message, Encoding encoding)
{
    if (encoding == Encoding::Cbor) {
        return Json::to_cbor(message);
    }
    // Replace invalid UTF-8 (a property can hold anything) instead of throwing.
    const std::string text
        = binaryToTagged(message).dump(-1, ' ', false, Json::error_handler_t::replace);
    return {text.begin(), text.end()};
}

std::string base64Encode(const std::uint8_t* data, std::size_t size)
{
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (i < size) {
        std::uint32_t n = data[i] << 16;
        if (i + 1 < size) {
            n |= data[i + 1] << 8;
        }
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += i + 1 < size ? alphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

Bytes base64Decode(const std::string& text)
{
    std::array<int, 256> table {};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(alphabet[i])] = i;
    }
    // Accept the URL-safe alphabet too.
    table['-'] = 62;
    table['_'] = 63;

    Bytes out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : text) {
        const int v = table[static_cast<unsigned char>(ch)];
        if (v < 0) {
            if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ') {
                continue;
            }
            throw std::invalid_argument("invalid base64 character");
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

Json makeBinary(const void* data, std::size_t size)
{
    const auto* begin = static_cast<const std::uint8_t*>(data);
    return Json::binary(Bytes(begin, begin + size));
}

}  // namespace Api
