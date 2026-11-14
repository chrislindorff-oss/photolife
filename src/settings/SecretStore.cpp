#include "settings/SecretStore.h"

#ifdef PHOTOLIFE_HAVE_KEYCHAIN
#include <qtkeychain/keychain.h>

#include <QDebug>
#include <QEventLoop>
#endif

namespace pl {

#ifdef PHOTOLIFE_HAVE_KEYCHAIN
namespace {

const QString kService = QStringLiteral("PhotoLife");

// Runs a QtKeychain job to the end (they answer through the event loop).
void runJob(QKeychain::Job &job)
{
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.setAutoDelete(false);
    job.start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

class KeychainSecretStore : public SecretStore
{
public:
    bool available() override
    {
        if (!m_available) {
            // Reading something that isn't there says whether there's a keychain.
            QKeychain::ReadPasswordJob probe(kService);
            probe.setKey(QStringLiteral("photolife/probe"));
            runJob(probe);
            m_available = probe.error() == QKeychain::NoError || probe.error() == QKeychain::EntryNotFound;
            if (!*m_available)
                qInfo() << "System keychain not available:" << probe.errorString();
        }
        return *m_available;
    }

    std::optional<QString> read(const QString &key) override
    {
        if (const auto it = m_cache.constFind(key); it != m_cache.constEnd())
            return *it;
        if (!available())
            return std::nullopt;
        QKeychain::ReadPasswordJob job(kService);
        job.setKey(key);
        runJob(job);
        std::optional<QString> value;
        if (job.error() == QKeychain::NoError)
            value = job.textData();
        else if (job.error() != QKeychain::EntryNotFound)
            qWarning() << "Couldn't read" << key << "from the keychain:" << job.errorString();
        m_cache.insert(key, value);
        return value;
    }

    bool write(const QString &key, const QString &value) override
    {
        if (!available())
            return false;
        if (value.isEmpty()) {
            QKeychain::DeletePasswordJob job(kService);
            job.setKey(key);
            runJob(job);
            if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
                qWarning() << "Couldn't remove" << key << "from the keychain:" << job.errorString();
                return false;
            }
            m_cache.insert(key, std::nullopt);
            return true;
        }
        QKeychain::WritePasswordJob job(kService);
        job.setKey(key);
        job.setTextData(value);
        runJob(job);
        if (job.error() != QKeychain::NoError) {
            qWarning() << "Couldn't save" << key << "in the keychain:" << job.errorString();
            return false;
        }
        m_cache.insert(key, value);
        return true;
    }

private:
    std::optional<bool> m_available;
    QHash<QString, std::optional<QString>> m_cache;
};

} // namespace
#endif

std::unique_ptr<SecretStore> makeSystemSecretStore()
{
#ifdef PHOTOLIFE_HAVE_KEYCHAIN
    return std::make_unique<KeychainSecretStore>();
#else
    return nullptr;
#endif
}

} // namespace pl
