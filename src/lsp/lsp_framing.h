#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace codetldr {

// LspFraming: Content-Length header parsing and message reassembly.
//
// LSP wire protocol uses HTTP-style framing:
//   Content-Length: N\r\n
//   \r\n
//   <N bytes of JSON body>
//
// Feed raw bytes from pipe read() calls into the buffer.
// Call extract() to attempt to retrieve one complete message.
// Multiple messages can be buffered; call extract() in a loop
// until it returns nullopt.
class LspFraming {
public:
    // Feed raw bytes from pipe read() into the internal buffer.
    void feed(const char* data, size_t len);
    void feed(const std::string& data);

    // Try to extract one complete message from the buffer.
    // Returns std::nullopt if not enough data yet.
    // On success, consumes the message from the buffer and returns parsed JSON.
    std::optional<nlohmann::json> extract();

    // Encode a JSON message with Content-Length header for writing.
    // Returns: "Content-Length: N\r\n\r\n{json_body}"
    static std::string encode(const nlohmann::json& msg);

    // Number of buffered bytes not yet consumed
    size_t buffered() const { return buffer_.size(); }

private:
    std::string buffer_;

    // State machine: either looking for header end (\r\n\r\n) or waiting for body bytes
    enum class State { HEADER, BODY };
    State state_ = State::HEADER;
    size_t content_length_ = 0;
};

} // namespace codetldr
