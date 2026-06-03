#include "JwtAuth.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

using json = nlohmann::json;

namespace jwt_auth {
namespace {

constexpr int ADMIN_JWT_EXPIRATION_MINUTES = 480;
const char* DEFAULT_ADMIN_SECRET = "super-secret-admin-key-change-in-production";
const char* DEFAULT_ADMIN_USERNAME = "admin";
const char* DEFAULT_ADMIN_PASSWORD_SHA256 = "6fb34a4fba55fe6159d89d072634203e92e5a501c850583c8bcdf07997ad0b1f";

std::string get_env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value && value[0]) ? value : fallback;
}

std::vector<unsigned char> sha256_bytes(const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_len = 0;
    DWORD data_len = 0;
    DWORD hash_len = 0;
    std::vector<unsigned char> hash_object;
    std::vector<unsigned char> hash_value;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    hash_object.resize(object_len);
    hash_value.resize(hash_len);

    if (BCryptCreateHash(alg, &hash, hash_object.data(), object_len, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptFinishHash(hash, hash_value.data(), hash_len, 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash_value;
}

std::vector<unsigned char> hmac_sha256_bytes(const std::string& key, const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_len = 0;
    DWORD data_len = 0;
    DWORD hash_len = 0;
    std::vector<unsigned char> hash_object;
    std::vector<unsigned char> hash_value;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    hash_object.resize(object_len);
    hash_value.resize(hash_len);

    if (BCryptCreateHash(
            alg,
            &hash,
            hash_object.data(),
            object_len,
            reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
            static_cast<ULONG>(key.size()),
            0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptFinishHash(hash, hash_value.data(), hash_len, 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash_value;
}

std::string to_hex(const std::vector<unsigned char>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char value : bytes) {
        out << std::setw(2) << static_cast<int>(value);
    }
    return out.str();
}

const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url_encode(const std::vector<unsigned char>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t block = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) block |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) block |= static_cast<uint32_t>(data[i + 2]);

        out.push_back(B64URL[(block >> 18) & 0x3f]);
        out.push_back(B64URL[(block >> 12) & 0x3f]);
        if (i + 1 < data.size()) out.push_back(B64URL[(block >> 6) & 0x3f]);
        if (i + 2 < data.size()) out.push_back(B64URL[block & 0x3f]);
    }
    return out;
}

std::optional<std::vector<unsigned char>> base64url_decode(const std::string& input) {
    std::vector<int> table(256, -1);
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(B64URL[i])] = i;

    std::vector<unsigned char> out;
    int val = 0;
    int bits = -8;
    for (unsigned char c : input) {
        if (table[c] == -1) return std::nullopt;
        val = (val << 6) + table[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string sign_hs256(const std::string& data) {
    std::string secret = get_env_or("ADMIN_JWT_SECRET", DEFAULT_ADMIN_SECRET);
    return base64url_encode(hmac_sha256_bytes(secret, data));
}

long long now_epoch_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

bool verify_admin_password(const std::string& username, const std::string& password) {
    if (username != DEFAULT_ADMIN_USERNAME) return false;
    return to_hex(sha256_bytes(password)) == DEFAULT_ADMIN_PASSWORD_SHA256;
}

AdminTokenResponse create_admin_token(const std::string& username) {
    json header = {
        {"alg", "HS256"},
        {"typ", "JWT"},
    };
    long long iat = now_epoch_seconds();
    long long exp = iat + static_cast<long long>(ADMIN_JWT_EXPIRATION_MINUTES) * 60LL;
    json payload = {
        {"sub", username},
        {"iat", iat},
        {"exp", exp},
    };

    const std::string header_dump = header.dump();
    const std::string header_part = base64url_encode(std::vector<unsigned char>(header_dump.begin(), header_dump.end()));
    const std::string payload_dump = payload.dump();
    const std::string payload_part = base64url_encode(std::vector<unsigned char>(payload_dump.begin(), payload_dump.end()));
    const std::string signing_input = header_part + "." + payload_part;
    const std::string sig_part = sign_hs256(signing_input);

    AdminTokenResponse response;
    response.access_token = signing_input + "." + sig_part;
    response.expires_in = ADMIN_JWT_EXPIRATION_MINUTES * 60;
    return response;
}

bool validate_admin_token(const std::string& token, std::string& username_out) {
    const size_t first_dot = token.find('.');
    const size_t second_dot = token.find('.', first_dot == std::string::npos ? first_dot : first_dot + 1);
    if (first_dot == std::string::npos || second_dot == std::string::npos) return false;

    const std::string header_part = token.substr(0, first_dot);
    const std::string payload_part = token.substr(first_dot + 1, second_dot - first_dot - 1);
    const std::string sig_part = token.substr(second_dot + 1);

    if (sign_hs256(header_part + "." + payload_part) != sig_part) return false;

    auto payload_bytes = base64url_decode(payload_part);
    if (!payload_bytes) return false;

    json payload;
    try {
        payload = json::parse(std::string(payload_bytes->begin(), payload_bytes->end()));
    } catch (...) {
        return false;
    }

    if (!payload.contains("sub") || !payload["sub"].is_string()) return false;
    if (!payload.contains("exp") || !payload["exp"].is_number()) return false;
    if (payload["exp"].get<long long>() < now_epoch_seconds()) return false;

    username_out = payload["sub"].get<std::string>();
    return true;
}

std::optional<std::string> extract_bearer_token(const std::string& authorization_header) {
    const std::string prefix = "Bearer ";
    if (authorization_header.size() < prefix.size()) return std::nullopt;
    if (!std::equal(prefix.begin(), prefix.end(), authorization_header.begin(), authorization_header.begin() + prefix.size(),
                    [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
        return std::nullopt;
    }
    std::string token = authorization_header.substr(prefix.size());
    if (token.empty()) return std::nullopt;
    return token;
}

int admin_expiration_seconds() {
    return ADMIN_JWT_EXPIRATION_MINUTES * 60;
}

} // namespace jwt_auth
