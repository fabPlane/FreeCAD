// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <Api/Codec.h>
#include <Api/Server.h>
#include <src/App/InitApplication.h>

using Api::Json;

class ApiServerTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        sink = server().addEventSink([this](const Json& event) { events.push_back(event); });
        const Json doc = call("NewDocument", {{"name", "ApiTest"}});
        docName = doc["result"]["name"].get<std::string>();
        events.clear();
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(docName.c_str());
        server().removeEventSink(sink);
    }

    static Api::Server& server()
    {
        return Api::Server::instance();
    }

    /// Round trip through bytes, as a transport does.
    Json call(
        const std::string& cmd,
        Json params = Json::object(),
        Api::Encoding encoding = Api::Encoding::Json
    )
    {
        Json request {{"id", ++id}, {"cmd", cmd}, {"params", std::move(params)}, {"client", "test"}};
        const auto bytes = Api::encode(request, encoding);
        std::size_t eventsBefore = events.size();
        Json reply;
        server().dispatch(bytes.data(), bytes.size(), [&](Api::Bytes&& out, Api::Encoding used) {
            EXPECT_EQ(used, encoding);
            // Events a request raises are delivered after its reply.
            EXPECT_EQ(events.size(), eventsBefore);
            reply = Api::decode(out.data(), out.size(), used);
        });
        EXPECT_EQ(reply["id"], id);
        return reply;
    }

    Json ok(const std::string& cmd, Json params = Json::object())
    {
        Json reply = call(cmd, std::move(params));
        EXPECT_EQ(reply["status"], "OK") << cmd << ": " << reply.dump();
        return reply["result"];
    }

    std::vector<std::string> eventNames() const
    {
        std::vector<std::string> names;
        for (const auto& event : events) {
            names.push_back(event["event"].get<std::string>());
        }
        return names;
    }

    int id = 0;
    int sink = 0;
    std::string docName;
    std::vector<Json> events;
};

TEST_F(ApiServerTest, pingAndVersion)
{
    EXPECT_TRUE(ok("Ping").is_null());
    EXPECT_EQ(ok("GetVersion")["api"], Api::ProtocolVersion);
    EXPECT_EQ(call("Ping")["token"], server().token());
}

TEST_F(ApiServerTest, errors)
{
    EXPECT_EQ(call("NoSuchCommand")["status"], "UNKNOWN_COMMAND");
    EXPECT_EQ(call("GetObject", {{"doc", docName}, {"object", "Missing"}})["status"], "NOT_FOUND");
    EXPECT_EQ(call("GetObject", {{"doc", docName}})["status"], "BAD_REQUEST");

    Json request {{"id", 1}, {"cmd", "Ping"}, {"token", "not-this-server"}};
    EXPECT_EQ(server().dispatchMessage(request)["status"], "TOKEN_MISMATCH");

    const std::string garbage = "{not json";
    const auto reply
        = server().dispatch(reinterpret_cast<const std::uint8_t*>(garbage.data()), garbage.size());
    const Json decoded = Api::decode(reply.data(), reply.size(), Api::Encoding::Json);
    EXPECT_EQ(decoded["status"], "BAD_REQUEST");
}

TEST_F(ApiServerTest, cborRequestsGetCborReplies)
{
    const Json reply = call("Ping", Json::object(), Api::Encoding::Cbor);
    EXPECT_EQ(reply["status"], "OK");
}

TEST_F(ApiServerTest, addObjectSetPropertiesAndUndo)
{
    const Json info
        = ok("AddObject", {{"doc", docName}, {"type", "App::FeatureTest"}, {"label", "Probe"}});
    const std::string name = info["name"].get<std::string>();
    EXPECT_EQ(info["label"], "Probe");
    const auto names = eventNames();
    EXPECT_NE(std::find(names.begin(), names.end(), "ObjectCreated"), names.end());

    const Json changed = ok(
        "SetProperties",
        {{"doc", docName},
         {"object", name},
         {"values",
          {{"Integer", 42},
           {"String", "hello"},
           {"Enum", "Two"},
           {"Distance", "2 in"},
           {"Vector", {{"$type", "Vector"}, {"x", 1}, {"y", 2}, {"z", 3}}},
           {"Placement",
            {{"$type", "Placement"}, {"base", {10, 0, 0}}, {"axis", {0, 0, 1}}, {"angle", 90}}}}}}
    );
    ASSERT_EQ(changed.size(), 6U);

    const Json props = ok(
        "GetProperties",
        {{"doc", docName},
         {"object", name},
         {"names", {"Integer", "String", "Enum", "Distance", "Vector", "Placement"}}}
    );
    EXPECT_EQ(props[0]["value"], 42);
    EXPECT_EQ(props[1]["value"], "hello");
    EXPECT_EQ(props[2]["value"], "Two");
    EXPECT_FALSE(props[2]["enum"].empty());
    EXPECT_NEAR(props[3]["value"]["value"].get<double>(), 50.8, 1e-9);
    EXPECT_EQ(props[3]["unit"], "mm");
    EXPECT_EQ(props[4]["value"]["y"], 2.0);
    EXPECT_NEAR(props[5]["value"]["angle"].get<double>(), 90.0, 1e-9);
    EXPECT_EQ(props[5]["value"]["base"][0], 10.0);

    // Each editing command was its own undo step.
    const Json stack = ok("GetUndoStack", {{"doc", docName}});
    ASSERT_EQ(stack["undo"].size(), 2U);
    ok("Undo", {{"doc", docName}});
    const Json after
        = ok("GetProperties", {{"doc", docName}, {"object", name}, {"names", {"Integer"}}});
    EXPECT_NE(after[0]["value"], 42);
}

TEST_F(ApiServerTest, readOnlyPropertiesAreRefused)
{
    const Json info = ok("AddObject", {{"doc", docName}, {"type", "App::FeatureTest"}});
    const Json reply = call(
        "SetProperties",
        {{"doc", docName}, {"object", info["name"]}, {"values", {{"TypeReadOnly", 1}}}}
    );
    EXPECT_EQ(reply["status"], "FORBIDDEN");
}

TEST_F(ApiServerTest, transactionsGroupChanges)
{
    ok("OpenTransaction", {{"doc", docName}, {"name", "Two objects"}});
    ok("AddObject", {{"doc", docName}, {"type", "App::FeatureTest"}});
    ok("AddObject", {{"doc", docName}, {"type", "App::FeatureTest"}});
    ok("CommitTransaction", {{"doc", docName}});
    const Json stack = ok("GetUndoStack", {{"doc", docName}});
    ASSERT_EQ(stack["undo"].size(), 1U);
    EXPECT_EQ(stack["undo"][0], "Two objects");
    ok("Undo", {{"doc", docName}});
    EXPECT_TRUE(ok("GetObjects", {{"doc", docName}}).empty());
}

TEST_F(ApiServerTest, groupsNestChildren)
{
    const Json group = ok("AddObject", {{"doc", docName}, {"type", "App::DocumentObjectGroup"}});
    const Json child
        = ok("AddObject", {{"doc", docName}, {"type", "App::FeatureTest"}, {"group", group["name"]}});
    const Json objects = ok("GetObjects", {{"doc", docName}});
    ASSERT_EQ(objects.size(), 2U);
    EXPECT_EQ(objects[0]["children"], Json::array({child["name"]}));
    EXPECT_EQ(objects[1]["parents"], Json::array({group["name"]}));
}

TEST_F(ApiServerTest, objectChangedIsCoalescedPerRequest)
{
    const Json info = ok("AddObject", {{"doc", docName}, {"type", "App::FeatureTest"}});
    events.clear();
    ok("RunPython",
       {{"code",
         "o = App.getDocument('" + docName + "')." + info["name"].get<std::string>()
             + "\nfor i in range(5): o.Integer = i"}});
    int integerChanges = 0;
    for (const auto& event : events) {
        if (event["event"] == "ObjectChanged" && event["data"]["property"] == "Integer") {
            ++integerChanges;
            EXPECT_EQ(event["data"]["client"], "test");
        }
    }
    EXPECT_EQ(integerChanges, 1);
}

TEST_F(ApiServerTest, runPythonCapturesOutput)
{
    const Json printed
        = ok("RunPython", {{"code", "print('out')\nimport sys\nprint('err', file=sys.stderr)"}});
    EXPECT_EQ(printed["stdout"], "out\n");
    EXPECT_EQ(printed["stderr"], "err\n");

    const Json value = ok("RunPython", {{"code", "App.Vector(1, 2, 3)"}, {"mode", "auto"}});
    EXPECT_EQ(value["result"]["$type"], "Vector");

    const Json failed = ok("RunPython", {{"code", "raise ValueError('boom')"}});
    EXPECT_NE(failed["exception"].get<std::string>().find("ValueError: boom"), std::string::npos);

    server().options().allowPython = false;
    EXPECT_EQ(call("RunPython", {{"code", "1"}})["status"], "FORBIDDEN");
    server().options().allowPython = true;
}

TEST_F(ApiServerTest, saveAndOpenBytes)
{
    ok("AddObject", {{"doc", docName}, {"type", "App::FeatureTest"}, {"label", "Saved"}});
    const Json saved = ok("SaveDocumentBytes", {{"doc", docName}});
    ASSERT_TRUE(saved["data"].is_binary());
    EXPECT_GT(saved["data"].get_binary().size(), 100U);

    const Json opened = ok("OpenDocumentBytes", {{"data", saved["data"]}, {"fileName", "Copy.FCStd"}});
    const std::string copy = opened["name"].get<std::string>();
    EXPECT_NE(copy, docName);
    const Json objects = ok("GetObjects", {{"doc", copy}});
    ASSERT_EQ(objects.size(), 1U);
    EXPECT_EQ(objects[0]["label"], "Saved");
    App::GetApplication().closeDocument(copy.c_str());
}
