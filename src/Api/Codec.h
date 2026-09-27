// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ApiGlobal.h"

namespace Api
{

using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;

/// How a message is serialized on the wire. The server answers in the encoding it was asked in.
enum class Encoding
{
    Json,
    Cbor
};

/// A JSON message starts with '{'; anything else is taken as CBOR.
ApiExport Encoding detectEncoding(const std::uint8_t* data, std::size_t size);

/**
 * Parse a message. Binary data is a CBOR byte string in CBOR and {"$bytes": "<base64>"} in
 * JSON; both come back as a Json binary value, so handlers never see the difference.
 * Throws nlohmann::json::exception on malformed input.
 */
ApiExport Json decode(const std::uint8_t* data, std::size_t size, Encoding encoding);

/// Serialize a message; Json binary values become {"$bytes": ...} in JSON text.
ApiExport Bytes encode(const Json& message, Encoding encoding);

ApiExport std::string base64Encode(const std::uint8_t* data, std::size_t size);
ApiExport Bytes base64Decode(const std::string& text);

/// Wrap raw memory as a Json binary value.
ApiExport Json makeBinary(const void* data, std::size_t size);

template<typename T>
Json makeBinary(const std::vector<T>& values)
{
    return makeBinary(values.data(), values.size() * sizeof(T));
}

}  // namespace Api
