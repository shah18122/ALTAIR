// app/credential_helper_main.cpp -- OS-vault setup boundary for the desktop.
//
// Input is a small, versioned line protocol on stdin. Values are base64 only
// to make framing unambiguous; this is a private pipe, not encryption. Nothing
// secret is accepted in argv or emitted on stdout/stderr.

#include <broker/credential_store.hpp>

#include <array>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMaxCommandBytes = 20 * 1024;
constexpr std::string_view kMagic = "ALTAIR-CREDENTIAL/1";

std::optional<std::string> read_command() {
    std::string input;
    input.reserve(1024);
    std::size_t line_count = 0;
    std::size_t required_lines = 0;
    for (int next = std::getc(stdin); next != EOF; next = std::getc(stdin)) {
        if (input.size() == kMaxCommandBytes) return std::nullopt;
        input.push_back(static_cast<char>(next));
        if (next != '\n') continue;
        ++line_count;
        if (line_count == 2) {
            const auto first_end = input.find('\n');
            const std::string_view operation{
                input.data() + first_end + 1,
                input.size() - first_end - 2};
            required_lines = operation == "SAVE" ? 6 : 3;
        }
        if (required_lines != 0 && line_count == required_lines) break;
    }
    return input;
}

std::vector<std::string_view> lines(std::string_view input) {
    std::vector<std::string_view> result;
    while (!input.empty()) {
        const auto end = input.find('\n');
        auto line = input.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        result.push_back(line);
        if (end == std::string_view::npos) break;
        input.remove_prefix(end + 1);
    }
    while (!result.empty() && result.back().empty()) result.pop_back();
    return result;
}

int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::optional<std::string> decode64(std::string_view encoded) {
    if (encoded.empty() || encoded.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(encoded.size() / 4 * 3);
    for (std::size_t at = 0; at < encoded.size(); at += 4) {
        std::array<int, 4> value{};
        int padding = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned char c = static_cast<unsigned char>(encoded[at + i]);
            if (c == '=') {
                ++padding;
                value[i] = 0;
            } else {
                if (padding != 0) return std::nullopt;
                value[i] = base64_value(c);
                if (value[i] < 0) return std::nullopt;
            }
        }
        if (padding > 2 || (padding != 0 && at + 4 != encoded.size()))
            return std::nullopt;
        out.push_back(static_cast<char>((value[0] << 2) | (value[1] >> 4)));
        if (padding < 2)
            out.push_back(static_cast<char>((value[1] << 4) | (value[2] >> 2)));
        if (padding == 0)
            out.push_back(static_cast<char>((value[2] << 6) | value[3]));
    }
    if (out.empty() || out.size() > altair::broker::kMaxCredentialSecretBytes)
        return std::nullopt;
    return out;
}

struct BrokerKeys final {
    std::string service;
    std::array<std::string, 3> accounts;
};

std::optional<BrokerKeys> keys(std::string_view broker) {
    if (broker == "FYERS")
        return BrokerKeys{"altair.fyers",
                          {"client-id", "app-secret", "redirect-uri"}};
    if (broker == "KITE")
        return BrokerKeys{"altair.kite",
                          {"api-key", "api-secret", "redirect-uri"}};
    return std::nullopt;
}

void wipe(std::string& value) {
    volatile char* byte = value.data();
    for (std::size_t i = 0; i < value.size(); ++i) byte[i] = 0;
    value.clear();
}

const char* error_name(altair::broker::CredentialError error) {
    using altair::broker::CredentialError;
    switch (error) {
    case CredentialError::InvalidKey: return "invalid-key";
    case CredentialError::EmptySecret: return "empty-value";
    case CredentialError::TooLong: return "value-too-long";
    case CredentialError::NotFound: return "not-found";
    case CredentialError::Unsupported: return "vault-unavailable";
    case CredentialError::PlatformFailure: return "vault-failure";
    }
    return "unknown";
}

altair::broker::CredentialKey key(const BrokerKeys& keys, std::size_t index) {
    return {keys.service, keys.accounts[index]};
}

int status(const BrokerKeys& broker_keys) {
    bool complete = true;
    for (std::size_t i = 0; i < broker_keys.accounts.size(); ++i) {
        auto value = altair::broker::load_credential(key(broker_keys, i));
        if (!value) {
            complete = false;
        } else {
            wipe(*value);
        }
    }
    std::printf("%.*s %s\n", static_cast<int>(kMagic.size()), kMagic.data(),
                complete ? "OK complete" : "OK incomplete");
    return 0;
}

int erase(const BrokerKeys& broker_keys) {
    for (std::size_t i = 0; i < broker_keys.accounts.size(); ++i) {
        const auto result = altair::broker::erase_credential(key(broker_keys, i));
        if (!result && result.error() != altair::broker::CredentialError::NotFound) {
            std::printf("%.*s ERROR %s\n", static_cast<int>(kMagic.size()),
                        kMagic.data(), error_name(result.error()));
            return 3;
        }
    }
    std::printf("%.*s OK deleted\n", static_cast<int>(kMagic.size()), kMagic.data());
    return 0;
}

int save(const BrokerKeys& broker_keys,
         std::array<std::string, 3>& values) {
    // Save/replace all three. If one write fails, restore the preceding items
    // to their prior vault values so the UI never reports a half-saved setup.
    std::array<std::optional<std::string>, 3> previous;
    for (std::size_t i = 0; i < previous.size(); ++i) {
        auto loaded = altair::broker::load_credential(key(broker_keys, i));
        if (loaded) previous[i] = std::move(*loaded);
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto stored = altair::broker::store_credential(
            key(broker_keys, i), values[i]);
        if (!stored) {
            for (std::size_t rollback = 0; rollback < i; ++rollback) {
                if (previous[rollback]) {
                    (void)altair::broker::store_credential(
                        key(broker_keys, rollback), *previous[rollback]);
                } else {
                    (void)altair::broker::erase_credential(
                        key(broker_keys, rollback));
                }
            }
            for (auto& value : values) wipe(value);
            for (auto& value : previous) if (value) wipe(*value);
            std::printf("%.*s ERROR %s\n", static_cast<int>(kMagic.size()),
                        kMagic.data(), error_name(stored.error()));
            return 3;
        }
    }
    for (auto& value : values) wipe(value);
    for (auto& value : previous) if (value) wipe(*value);
    std::printf("%.*s OK saved\n", static_cast<int>(kMagic.size()), kMagic.data());
    return 0;
}

} // namespace

int main() {
    const auto input = read_command();
    if (!input) {
        std::printf("ALTAIR-CREDENTIAL/1 ERROR input-too-large\n");
        return 2;
    }
    const auto frame = lines(*input);
    if (frame.size() < 3 || frame[0] != kMagic) {
        std::printf("ALTAIR-CREDENTIAL/1 ERROR malformed-frame\n");
        return 2;
    }
    const auto broker_keys = keys(frame[2]);
    if (!broker_keys) {
        std::printf("ALTAIR-CREDENTIAL/1 ERROR unknown-broker\n");
        return 2;
    }
    if (frame[1] == "STATUS" && frame.size() == 3) return status(*broker_keys);
    if (frame[1] == "DELETE" && frame.size() == 3) return erase(*broker_keys);
    if (frame[1] != "SAVE" || frame.size() != 6) {
        std::printf("ALTAIR-CREDENTIAL/1 ERROR malformed-command\n");
        return 2;
    }
    std::array<std::string, 3> values;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto decoded = decode64(frame[i + 3]);
        if (!decoded) {
            for (auto& value : values) wipe(value);
            std::printf("ALTAIR-CREDENTIAL/1 ERROR malformed-value\n");
            return 2;
        }
        values[i] = *decoded;
    }
    return save(*broker_keys, values);
}
