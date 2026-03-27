#include "lsp/lsp_framing.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace codetldr {

void LspFraming::feed(const char* data, size_t len) {
    buffer_.append(data, len);
}

void LspFraming::feed(const std::string& data) {
    buffer_.append(data);
}

std::optional<nlohmann::json> LspFraming::extract() {
    if (state_ == State::HEADER) {
        // Search for the header/body separator: \r\n\r\n
        auto pos = buffer_.find("\r\n\r\n");
        if (pos == std::string::npos) {
            return std::nullopt;
        }

        // Parse headers from buffer_[0..pos)
        std::string header_block = buffer_.substr(0, pos);
        content_length_ = 0;

        // Split by \r\n and find Content-Length line
        size_t start = 0;
        while (start <= header_block.size()) {
            auto line_end = header_block.find("\r\n", start);
            std::string line;
            if (line_end == std::string::npos) {
                line = header_block.substr(start);
                start = header_block.size() + 1;
            } else {
                line = header_block.substr(start, line_end - start);
                start = line_end + 2;
            }

            // Case-insensitive comparison for "content-length:"
            // Standard says header names are case-insensitive.
            if (line.size() > 16) {
                std::string lower_prefix = line.substr(0, 16);
                std::transform(lower_prefix.begin(), lower_prefix.end(),
                               lower_prefix.begin(), ::tolower);
                if (lower_prefix == "content-length: ") {
                    content_length_ = static_cast<size_t>(
                        std::stoull(line.substr(16)));
                }
            }
        }

        if (content_length_ == 0) {
            // Malformed — skip this header block
            buffer_.erase(0, pos + 4);
            return std::nullopt;
        }

        // Erase header + separator from buffer
        buffer_.erase(0, pos + 4);
        state_ = State::BODY;
        // Fall through to body check below
    }

    if (state_ == State::BODY) {
        if (buffer_.size() < content_length_) {
            return std::nullopt;
        }

        // Extract exactly content_length_ bytes as the body
        std::string body = buffer_.substr(0, content_length_);
        buffer_.erase(0, content_length_);

        // Reset state for next message
        state_ = State::HEADER;
        content_length_ = 0;

        return nlohmann::json::parse(body);
    }

    return std::nullopt;
}

std::string LspFraming::encode(const nlohmann::json& msg) {
    std::string body = msg.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

} // namespace codetldr
