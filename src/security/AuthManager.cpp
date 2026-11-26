#include "AuthManager.h"

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#pragma comment(lib, "Advapi32.lib")
#endif

namespace
{
constexpr std::size_t BEARER_TOKEN_BYTES = 32;

bool sha256(const std::string& value, std::vector<std::uint8_t>& digest)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> result{};
    if (SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), result.data()) == nullptr)
        return false;

    digest.assign(result.begin(), result.end());
    return digest.size() == SHA256_DIGEST_LENGTH;
}

bool constantTimeEqual(const std::vector<std::uint8_t>& left,
                       const std::vector<std::uint8_t>& right)
{
    if (left.size() != right.size())
        return false;
    return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

std::string parseBearer(const std::string& header)
{
    constexpr const char* prefix = "Authorization: Bearer ";

    std::string line = header;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();

    if (line.find('\r') != std::string::npos ||
        line.find('\n') != std::string::npos)
        return {};

    if (line.rfind(prefix, 0) != 0)
        return {};

    std::string value = line.substr(std::char_traits<char>::length(prefix));
    if (value.empty() || value.find_first_of(" \t") != std::string::npos)
        return {};

    return value;
}
}

AuthManager::AuthManager(Mode mode, std::string bearerToken)
    : authMode(mode)
{
    if (validateBearerTokenFormat(bearerToken))
    {
        std::vector<std::uint8_t> digest;
        if (sha256(bearerToken, digest) && digest.size() == SHA256_DIGEST_LENGTH)
        {
            secretHash = std::move(digest);
            configured = true;
        }
    }

    volatile char* secure = reinterpret_cast<volatile char*>(bearerToken.data());
    for (std::size_t index = 0; index < bearerToken.size(); ++index)
        secure[index] = 0;
}

AuthManager AuthManager::fromBearerToken(std::string bearerToken, Mode mode)
{
    return AuthManager(mode, std::move(bearerToken));
}

AuthManager AuthManager::fromEnvironment()
{
    const char* modeText = std::getenv("PAYBRIDGE_AUTH_MODE");

#ifdef _WIN32
    if (modeText && std::string(modeText) == "production")
    {
        const char* credentialTarget = std::getenv("PAYBRIDGE_CREDENTIAL_TARGET");
        if (!credentialTarget || std::string(credentialTarget).empty())
            return AuthManager(Mode::PRODUCTION, {});

        PCREDENTIALA credential = nullptr;
        if (!CredReadA(credentialTarget, CRED_TYPE_GENERIC, 0, &credential))
            return AuthManager(Mode::PRODUCTION, {});

        std::string bearer;
        if (credential->CredentialBlobSize > 0)
        {
            bearer.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                          credential->CredentialBlobSize);
        }

        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
        CredFree(credential);

        return AuthManager(Mode::PRODUCTION, bearer);
    }
#endif

    if (modeText && std::string(modeText) == "development")
    {
        const char* token = std::getenv("PAYBRIDGE_DEV_AUTH_TOKEN");
        if (token)
            return AuthManager(Mode::DEVELOPMENT, token);
    }

    return AuthManager(Mode::PRODUCTION, {});
}

AuthManager::Mode AuthManager::mode() const noexcept
{
    return authMode;
}

AuthManager::VerificationResult AuthManager::verifyAuthorization(
    const std::string& authorizationHeader) const
{
    if (!configured)
        return VerificationResult::NOT_CONFIGURED;

    if (authorizationHeader.empty())
        return VerificationResult::MISSING;

    try
    {
        const std::string bearer = parseBearer(authorizationHeader);
        if (bearer.empty())
            return VerificationResult::MALFORMED;

        if (!validateBearerTokenFormat(bearer))
            return VerificationResult::MALFORMED;

        std::vector<std::uint8_t> presentedHash;
        if (!sha256(bearer, presentedHash))
            return VerificationResult::NOT_CONFIGURED;

        if (!constantTimeEqual(presentedHash, secretHash))
            return VerificationResult::INVALID;

        return VerificationResult::ALLOWED;
    }
    catch (...)
    {
        return VerificationResult::NOT_CONFIGURED;
    }
}

std::string AuthManager::generateBearerToken()
{
    std::array<unsigned char, BEARER_TOKEN_BYTES> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        return {};

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : random)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

bool AuthManager::validateBearerTokenFormat(const std::string& bearerToken) noexcept
{
    if (bearerToken.size() != BEARER_TOKEN_BYTES * 2)
        return false;

    return std::all_of(bearerToken.begin(), bearerToken.end(), [](unsigned char character)
    {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

bool AuthManager::isConfigured() const noexcept
{
    return configured;
}
