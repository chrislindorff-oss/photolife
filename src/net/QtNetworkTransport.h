#pragma once

#include "net/Transport.h"

#include <QObject>

class QNetworkAccessManager;

namespace pl::net {

// Transport backed by QNetworkAccessManager. One instance can serve many
// requests; each reply is cleaned up when it finishes.
class QtNetworkTransport : public QObject, public Transport
{
    Q_OBJECT

public:
    explicit QtNetworkTransport(QObject *parent = nullptr);
    ~QtNetworkTransport() override;

    void send(const Request &request, Callback callback) override;

    // How long a request may go without a byte moving either way before it's
    // given up as a failure (which the callers' retries then handle): a GET,
    // and anything with a body (an upload, or a photo being scored, where
    // the server thinks for a while before answering).
    void setIdleTimeouts(int getMs, int sendMs);
    static constexpr int kGetIdleMs = 30000;
    static constexpr int kSendIdleMs = 60000;

private:
    QNetworkAccessManager *m_nam;
    int m_getIdleMs = kGetIdleMs;
    int m_sendIdleMs = kSendIdleMs;
};

} // namespace pl::net
