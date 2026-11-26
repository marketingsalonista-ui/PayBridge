#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class AuthManager
{
public:
    enum class Mode
    {
        DEVELOPMENT,
        PRODUCTION
    };

    enum class VerificationResult
    {
        ALLOWED,
        MISSING,
        MALFORMED,
        INVALID,
        NOT_CONFIGURED
    };

    static AuthManager fromBearerToken(
        std::string bearerToken,
        Mode mode = Mode::DEVELOPMENT
    );

    static AuthManager fromEnvironment();

    Mode mode() const noexcept;

    VerificationResult verifyAuthorization(
        const std::string& authorizationHeader
    ) const;

    static std::string generateBearerToken();

    static bool validateBearerTokenFormat(
        const std::string& bearerToken
    ) noexcept;

    bool isConfigured() const noexcept;

private:
    AuthManager(Mode mode, std::string bearerToken);

    Mode authMode = Mode::PRODUCTION;
    bool configured = false;
    std::vector<std::uint8_t> secretHash;
};
