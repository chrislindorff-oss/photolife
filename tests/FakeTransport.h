#pragma once

#include <QList>
#include <QTimer>

#include <functional>

#include "net/Transport.h"

// Scripted Transport for tests: `responder(request, callIndex)` decides each
// reply; every request is recorded and answered on the next event-loop turn.
class FakeTransport : public pl::net::Transport
{
public:
    std::function<Reply(const Request &, int)> responder;
    QList<Request> received;

    void send(const Request &request, Callback callback) override
    {
        const int index = received.size();
        received.append(request);
        Reply reply = responder ? responder(request, index) : Reply{};
        QTimer::singleShot(0, [callback = std::move(callback), reply = std::move(reply)] {
            callback(reply);
        });
    }

    static Reply ok(const QByteArray &body, const QString &etag = {})
    {
        Reply r;
        r.status = 200;
        r.body = body;
        r.etag = etag;
        return r;
    }

    static Reply httpStatus(int status, const QByteArray &body = {})
    {
        Reply r;
        r.status = status;
        r.body = body;
        return r;
    }
};
