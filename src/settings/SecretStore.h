#pragma once

#include <QHash>
#include <QString>

#include <memory>
#include <optional>

namespace pl {

// Where passwords and tokens are kept: the system keychain (GNOME Keyring /
// KWallet, macOS Keychain, Windows Credential Manager) through QtKeychain.
// Synchronous on purpose -- Settings' getters are -- with what's been read
// cached, so the keychain is asked once per secret per run. Keys are the
// settings keys they replace ("inat/apiToken", "servers/<id>/pg/password"...).
class SecretStore
{
public:
    virtual ~SecretStore() = default;
    // Whether there's a keychain to use (a backend, unlocked or unlockable).
    virtual bool available() = 0;
    // nullopt: nothing stored (or it couldn't be read).
    virtual std::optional<QString> read(const QString &key) = 0;
    // An empty value removes it. False if the keychain wouldn't take it.
    virtual bool write(const QString &key, const QString &value) = 0;
};

// The system keychain; nullptr when PhotoLife is built without QtKeychain.
std::unique_ptr<SecretStore> makeSystemSecretStore();

// In memory, for tests (and to stand in for an unavailable keychain).
class MemorySecretStore : public SecretStore
{
public:
    explicit MemorySecretStore(bool available = true) : m_available(available) {}
    bool available() override { return m_available; }
    std::optional<QString> read(const QString &key) override
    {
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool write(const QString &key, const QString &value) override
    {
        if (!m_available)
            return false;
        if (value.isEmpty())
            m_values.remove(key);
        else
            m_values.insert(key, value);
        return true;
    }
    const QHash<QString, QString> &values() const { return m_values; }

private:
    bool m_available;
    QHash<QString, QString> m_values;
};

} // namespace pl
