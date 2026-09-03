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

private:
    QNetworkAccessManager *m_nam;
};

} // namespace pl::net
