// Test: LspFraming Content-Length header parsing and message reassembly
// Tests:
//   1. complete_single_message: feed full message in one call
//   2. partial_header: feed header in two chunks
//   3. partial_body: feed header + partial body, then rest
//   4. multiple_messages: feed two concatenated messages
//   5. large_message_200kb: feed 200KB body in 4KB chunks
//   6. extra_headers_ignored: Content-Type header is ignored
//   7. encode_roundtrip: encode then feed back, get original JSON

#include "lsp/lsp_framing.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

// ============================================================
// Test 1: complete_single_message
// ============================================================
static void test_1_complete_single_message() {
    std::cout << "Test 1: complete_single_message..." << std::flush;

    codetldr::LspFraming framing;
    std::string msg = "Content-Length: 15\r\n\r\n{\"id\":1,\"ok\":1}";
    framing.feed(msg);

    auto result = framing.extract();
    assert(result.has_value() && "Test 1: should extract a message");
    assert((*result)["id"] == 1 && "Test 1: id should be 1");
    assert((*result)["ok"] == 1 && "Test 1: ok should be 1");

    // Buffer should be empty now
    assert(framing.buffered() == 0 && "Test 1: buffer should be empty after extraction");

    // Second extract should return nullopt
    auto result2 = framing.extract();
    assert(!result2.has_value() && "Test 1: second extract should return nullopt");

    std::cout << " PASS\n";
}

// ============================================================
// Test 2: partial_header
// ============================================================
static void test_2_partial_header() {
    std::cout << "Test 2: partial_header..." << std::flush;

    codetldr::LspFraming framing;

    // Feed first part of header — no complete message yet
    framing.feed("Content-Len");
    auto result1 = framing.extract();
    assert(!result1.has_value() && "Test 2: partial header should not yield message");

    // Feed rest of header + body
    framing.feed("gth: 15\r\n\r\n{\"id\":1,\"ok\":1}");
    auto result2 = framing.extract();
    assert(result2.has_value() && "Test 2: complete message should be extracted");
    assert((*result2)["id"] == 1 && "Test 2: id should be 1");
    assert((*result2)["ok"] == 1 && "Test 2: ok should be 1");

    std::cout << " PASS\n";
}

// ============================================================
// Test 3: partial_body
// ============================================================
static void test_3_partial_body() {
    std::cout << "Test 3: partial_body..." << std::flush;

    codetldr::LspFraming framing;
    std::string body = "{\"id\":1,\"ok\":1}"; // 15 bytes
    std::string header = "Content-Length: 15\r\n\r\n";

    // Feed header + first 7 bytes of body
    framing.feed(header + body.substr(0, 7));
    auto result1 = framing.extract();
    assert(!result1.has_value() && "Test 3: partial body should not yield message");

    // Feed remaining 7 bytes
    framing.feed(body.substr(7));
    auto result2 = framing.extract();
    assert(result2.has_value() && "Test 3: complete body should yield message");
    assert((*result2)["id"] == 1 && "Test 3: id should be 1");

    std::cout << " PASS\n";
}

// ============================================================
// Test 4: multiple_messages
// ============================================================
static void test_4_multiple_messages() {
    std::cout << "Test 4: multiple_messages..." << std::flush;

    codetldr::LspFraming framing;

    // Two complete messages concatenated
    std::string msg1 = "Content-Length: 15\r\n\r\n{\"id\":1,\"ok\":1}";
    std::string msg2 = "Content-Length: 15\r\n\r\n{\"id\":2,\"ok\":0}";
    framing.feed(msg1 + msg2);

    auto result1 = framing.extract();
    assert(result1.has_value() && "Test 4: first message should be extracted");
    assert((*result1)["id"] == 1 && "Test 4: first id should be 1");

    auto result2 = framing.extract();
    assert(result2.has_value() && "Test 4: second message should be extracted");
    assert((*result2)["id"] == 2 && "Test 4: second id should be 2");
    assert((*result2)["ok"] == 0 && "Test 4: second ok should be 0");

    auto result3 = framing.extract();
    assert(!result3.has_value() && "Test 4: no third message");

    std::cout << " PASS\n";
}

// ============================================================
// Test 5: large_message_200kb
// ============================================================
static void test_5_large_message_200kb() {
    std::cout << "Test 5: large_message_200kb..." << std::flush;

    // Build a JSON object with ~200KB data field
    // The string "AAAA..." repeated to fill ~200KB
    const size_t target_data_size = 200 * 1024; // 200KB of data
    std::string large_data(target_data_size, 'A');

    // Build the JSON body: {"data":"AAAA..."}
    // The actual JSON body size = len of '{"data":"' + data size + '"}' = 10 + data + 2
    nlohmann::json msg_json;
    msg_json["data"] = large_data;
    std::string body = msg_json.dump();

    // Build framed message
    std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    std::string full_message = header + body;

    // Feed in 4096-byte chunks
    const size_t chunk_size = 4096;
    codetldr::LspFraming framing;
    size_t offset = 0;
    std::optional<nlohmann::json> result;

    while (offset < full_message.size()) {
        size_t to_feed = std::min(chunk_size, full_message.size() - offset);
        framing.feed(full_message.data() + offset, to_feed);
        offset += to_feed;

        auto r = framing.extract();
        if (r.has_value()) {
            result = r;
            break;
        }
    }

    assert(result.has_value() && "Test 5: 200KB message should be extracted");
    assert((*result)["data"].get<std::string>() == large_data &&
           "Test 5: data field should match original");
    assert(framing.buffered() == 0 && "Test 5: buffer should be empty after extraction");

    std::cout << " PASS\n";
}

// ============================================================
// Test 6: extra_headers_ignored
// ============================================================
static void test_6_extra_headers_ignored() {
    std::cout << "Test 6: extra_headers_ignored..." << std::flush;

    codetldr::LspFraming framing;
    // Message with Content-Type header (should be ignored)
    std::string msg =
        "Content-Length: 15\r\n"
        "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
        "\r\n"
        "{\"id\":1,\"ok\":1}";
    framing.feed(msg);

    auto result = framing.extract();
    assert(result.has_value() && "Test 6: message should be extracted despite extra headers");
    assert((*result)["id"] == 1 && "Test 6: id should be 1");
    assert((*result)["ok"] == 1 && "Test 6: ok should be 1");

    std::cout << " PASS\n";
}

// ============================================================
// Test 7: encode_roundtrip
// ============================================================
static void test_7_encode_roundtrip() {
    std::cout << "Test 7: encode_roundtrip..." << std::flush;

    nlohmann::json original;
    original["method"] = "initialize";
    original["id"] = 42;
    original["params"]["rootPath"] = "/tmp/test";

    std::string framed = codetldr::LspFraming::encode(original);

    // Verify framed format: "Content-Length: N\r\n\r\n{body}"
    assert(framed.find("Content-Length: ") == 0 &&
           "Test 7: framed message should start with Content-Length");
    assert(framed.find("\r\n\r\n") != std::string::npos &&
           "Test 7: framed message should contain \\r\\n\\r\\n separator");

    codetldr::LspFraming framing;
    framing.feed(framed);
    auto result = framing.extract();
    assert(result.has_value() && "Test 7: encoded message should be extractable");
    assert((*result)["method"] == "initialize" && "Test 7: method should be initialize");
    assert((*result)["id"] == 42 && "Test 7: id should be 42");
    assert((*result)["params"]["rootPath"] == "/tmp/test" &&
           "Test 7: rootPath should match");

    std::cout << " PASS\n";
}

} // anonymous namespace

int main() {
    std::cout << "=== LspFraming Tests ===\n";

    try {
        test_1_complete_single_message();
        test_2_partial_header();
        test_3_partial_body();
        test_4_multiple_messages();
        test_5_large_message_200kb();
        test_6_extra_headers_ignored();
        test_7_encode_roundtrip();

        std::cout << "\nAll tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nFATAL: " << ex.what() << "\n";
        return 1;
    }
}
