// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Api/Codec.h>

using Api::Json;

namespace
{

Api::Bytes bytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

}  // namespace

TEST(ApiCodec, detectsJsonAndCbor)
{
    const auto json = bytes("  {\"cmd\":\"Ping\"}");
    EXPECT_EQ(Api::detectEncoding(json.data(), json.size()), Api::Encoding::Json);
    const auto cbor = Json::to_cbor(Json {{"cmd", "Ping"}});
    EXPECT_EQ(Api::detectEncoding(cbor.data(), cbor.size()), Api::Encoding::Cbor);
}

TEST(ApiCodec, base64RoundTripsEveryLength)
{
    for (std::size_t n = 0; n < 16; ++n) {
        Api::Bytes data(n);
        for (std::size_t i = 0; i < n; ++i) {
            data[i] = static_cast<std::uint8_t>(i * 37 + 250);
        }
        EXPECT_EQ(Api::base64Decode(Api::base64Encode(data.data(), data.size())), data) << n;
    }
    EXPECT_EQ(Api::base64Encode(reinterpret_cast<const std::uint8_t*>("Man"), 3), "TWFu");
    EXPECT_EQ(Api::base64Encode(reinterpret_cast<const std::uint8_t*>("Ma"), 2), "TWE=");
}

TEST(ApiCodec, binaryIsTaggedInJsonAndRawInCbor)
{
    const std::vector<float> values {1.5F, -2.0F};
    const Json message {{"result", {{"positions", Api::makeBinary(values)}}}};

    const auto json = Api::encode(message, Api::Encoding::Json);
    const std::string text(json.begin(), json.end());
    EXPECT_NE(text.find("\"$bytes\""), std::string::npos);
    const Json fromJson = Api::decode(json.data(), json.size(), Api::Encoding::Json);
    ASSERT_TRUE(fromJson["result"]["positions"].is_binary());
    EXPECT_EQ(fromJson["result"]["positions"].get_binary().size(), sizeof(float) * 2);

    const auto cbor = Api::encode(message, Api::Encoding::Cbor);
    const Json fromCbor = Api::decode(cbor.data(), cbor.size(), Api::Encoding::Cbor);
    EXPECT_EQ(fromCbor, fromJson);
}

TEST(ApiCodec, malformedJsonThrows)
{
    const auto bad = bytes("{\"cmd\":");
    EXPECT_ANY_THROW(Api::decode(bad.data(), bad.size(), Api::Encoding::Json));
}
