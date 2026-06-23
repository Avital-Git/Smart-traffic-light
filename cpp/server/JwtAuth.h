#pragma once

#include <optional>
#include <string>

namespace jwt_auth {

struct AdminTokenResponse {
    std::string access_token;
    std::string token_type = "bearer";
    int expires_in = 0;
};

bool verify_admin_password(const std::string& username, const std::string& password);
AdminTokenResponse create_admin_token(const std::string& username, const std::string& role = "regular_admin");
bool validate_admin_token(const std::string& token, std::string& username_out);
bool validate_admin_token_with_role(const std::string& token, std::string& username_out, std::string& role_out);
std::optional<std::string> extract_bearer_token(const std::string& authorization_header);
int admin_expiration_seconds();
std::string hash_password_with_salt(const std::string& salt, const std::string& password);
std::string generate_salt_hex(size_t num_random_bytes = 16);

} // namespace jwt_auth
