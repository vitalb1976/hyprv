#pragma once

#include <windows.h>
#include <wincred.h>
#include <string>
#include <cwctype>
#include <cstdint>
#pragma comment(lib, "Advapi32.lib")

namespace hyprv::credentials
{
    // Length-prefixed components avoid delimiter collisions; only the host
    // is case-folded (remote usernames may be case-sensitive).
    inline std::wstring Target(std::wstring address, uint16_t port,
                               std::wstring const& domain, std::wstring const& user)
    {
        for (auto& ch : address) ch = static_cast<wchar_t>(std::towlower(ch));
        auto part = [](std::wstring const& value) {
            return std::to_wstring(value.size()) + L":" + value;
        };
        return L"hyprv/rdp/" + part(address) + L"/" + std::to_wstring(port ? port : 3389)
            + L"/" + part(domain) + L"/" + part(user);
    }

    class StoredPassword
    {
    public:
        explicit StoredPassword(std::wstring const& target)
        {
            if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &m_credential))
                m_credential = nullptr;
        }
        ~StoredPassword()
        {
            if (m_credential)
            {
                if (m_credential->CredentialBlob && m_credential->CredentialBlobSize)
                    SecureZeroMemory(m_credential->CredentialBlob, m_credential->CredentialBlobSize);
                CredFree(m_credential);
            }
        }
        StoredPassword(StoredPassword const&) = delete;
        StoredPassword& operator=(StoredPassword const&) = delete;
        bool Exists() const { return m_credential != nullptr; }
        bool Valid() const
        {
            return m_credential && m_credential->CredentialBlob
                && m_credential->CredentialBlobSize > 0
                && m_credential->CredentialBlobSize % sizeof(wchar_t) == 0;
        }
        wchar_t const* Data() const
        { return reinterpret_cast<wchar_t const*>(m_credential->CredentialBlob); }
        UINT Length() const { return m_credential->CredentialBlobSize / sizeof(wchar_t); }
    private:
        PCREDENTIALW m_credential = nullptr;
    };

    struct PasswordInput
    {
        std::wstring value;
        ~PasswordInput() { if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); }
    };

    inline bool Save(std::wstring const& target, std::wstring const& user,
                     std::wstring const& password)
    {
        if (password.empty() || password.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE / sizeof(wchar_t))
        { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<wchar_t*>(target.c_str());
        credential.UserName = const_cast<wchar_t*>(user.c_str());
        credential.CredentialBlob = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(password.data()));
        credential.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(wchar_t));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE; // This user's local credential set.
        return CredWriteW(&credential, 0) != FALSE;
    }

    inline bool Remove(std::wstring const& target)
    {
        if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
        return GetLastError() == ERROR_NOT_FOUND;
    }
}
