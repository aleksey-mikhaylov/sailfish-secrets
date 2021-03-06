/*
 * Copyright (C) 2017 Jolla Ltd.
 * Contact: Chris Adams <chris.adams@jollamobile.com>
 * All rights reserved.
 * BSD 3-Clause License, see LICENSE.
 */

#include "requestqueue_p.h"
#include "logging_p.h"

#include "Secrets/secretsdaemonconnection.h"

#include <QtCore/QElapsedTimer>

#include <dbus/dbus.h>

Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestQueue(
        const QString &dbusObjectPath,
        const QString &dbusInterfaceName,
        Controller *parent,
        const QString &pluginDir,
        bool autotestMode)
    : QObject(parent)
    , m_controller(parent)
    , m_dbusObjectPath(dbusObjectPath)
    , m_dbusInterfaceName(dbusInterfaceName)
    , m_pluginDir(pluginDir)
    , m_autotestMode(autotestMode)
{
    qCDebug(lcSailfishSecretsDaemon) << "New API implementation request queue constructed:" << m_dbusObjectPath << "," << m_dbusInterfaceName;
}

Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::~RequestQueue()
{
}

void Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::handleClientConnection(const QDBusConnection &connection)
{
    QDBusConnection clientConnection(connection);
    if (!clientConnection.registerObject(m_dbusObjectPath,
#if QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)
                                         m_dbusInterfaceName,
#endif
                                         m_dbusObject,
                                         QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        qCWarning(lcSailfishSecretsDaemon) << "Could not register object for p2p connection!";
    } else {
        qCDebug(lcSailfishSecretsDaemon) << "Registered p2p object with the client connection!";
    }
}

void Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::handleRequest(
        int requestType,
        const QVariantList &inParams,
        const QDBusConnection &connection,
        const QDBusMessage &message,
        Sailfish::Crypto::Result &returnResult)
{
    // queue up a Sailfish Crypto API request
    DBusConnection *internalConnection = static_cast<DBusConnection*>(connection.internalPointer());
    unsigned long dbusRemotePid = 0;
    dbus_bool_t gotPid = dbus_connection_get_unix_process_id(internalConnection, &dbusRemotePid);
    if (!gotPid) {
        connection.send(message.createErrorReply(
                            QDBusError::Other,
                            QString::fromUtf8("Could not determine PID of caller to enforce access controls")));
    } else {
        Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData *data = new Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData;
        data->connection = connection;
        data->remotePid = (pid_t)dbusRemotePid;
        data->status = Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestPending;
        data->type = requestType;
        data->inParams = inParams;
        data->requestId = 0;
        Sailfish::Secrets::Result result = enqueueRequest(data);
        if (result.code() == Sailfish::Secrets::Result::Succeeded) {
            data->message = message;
            message.setDelayedReply(true);
        } else {
            Sailfish::Crypto::Result transformedResult(Sailfish::Crypto::Result::Failed);
            transformedResult.setErrorCode(Sailfish::Crypto::Result::DaemonError);
            transformedResult.setErrorMessage(result.errorMessage());
            returnResult = transformedResult;
            delete data;
        }
    }
}

void Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::handleRequest(
        int requestType,
        const QVariantList &inParams,
        const QDBusConnection &connection,
        const QDBusMessage &message,
        Sailfish::Secrets::Result &returnResult)
{
    // queue up a Sailfish Secrets API request
    DBusConnection *internalConnection = static_cast<DBusConnection*>(connection.internalPointer());
    unsigned long dbusRemotePid = 0;
    dbus_bool_t gotPid = dbus_connection_get_unix_process_id(internalConnection, &dbusRemotePid);
    if (!gotPid) {
        connection.send(message.createErrorReply(
                            QDBusError::Other,
                            QString::fromUtf8("Could not determine PID of caller to enforce access controls")));
    } else {
        Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData *data = new Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData;
        data->connection = connection;
        data->remotePid = (pid_t)dbusRemotePid;
        data->status = Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestPending;
        data->type = requestType;
        data->inParams = inParams;
        data->requestId = 0;
        Sailfish::Secrets::Result result = enqueueRequest(data);
        if (result.code() == Sailfish::Secrets::Result::Succeeded) {
            data->message = message;
            message.setDelayedReply(true);
        } else {
            returnResult = result;
            delete data;
        }
    }
}

void Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::handleRequest(
        pid_t callerPid,
        quint64 cryptoRequestId,
        int requestType,
        const QVariantList &inParams,
        Sailfish::Secrets::Result &result)
{
    // queue up a Secrets request as part of a Crypto request.
    Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData *data = new Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData;
    data->remotePid = callerPid;
    data->status = Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestPending;
    data->type = requestType;
    data->inParams = inParams;
    data->requestId = 0;
    data->isSecretsCryptoRequest = true;
    data->cryptoRequestId = cryptoRequestId;
    result = enqueueRequest(data);
    if (result.code() == Sailfish::Secrets::Result::Failed) {
        delete data;
    }
}

Sailfish::Secrets::Result Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::enqueueRequest(Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData *request)
{
    static quint64 requestId = 0;

    // If no free request ids (i.e. queue is full) then return an error to the client.
    quint64 prevId = requestId;
    quint64 nextFreeId = ++requestId;
    bool found = false;
    for ( ; nextFreeId != prevId; ++nextFreeId) {
        found = false;
        QList<Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData*>::const_iterator it = m_requests.constBegin();
        while (it != m_requests.constEnd()) {
            if ((*it)->requestId == nextFreeId) {
                // another current request is using this id.
                found = true;
                break;
            }
            it++;
        }
        if (!found) {
            // no requests in the queue are using this id.  it is free to use.
            break;
        }
    }

    if (found) {
        // all request ids are taken.  we cannot enqueue this request.
        qCWarning(lcSailfishSecretsDaemon) << "Cannot enqueue request:" << requestTypeToString(request->type) << ": queue is full!";
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::SecretsDaemonRequestQueueFullError,
                                         QString::fromUtf8("Request queue is full, try again later"));
    }

    qCDebug(lcSailfishSecretsDaemon) << "Enqueuing" << requestTypeToString(request->type) << "request with id:" << nextFreeId;
    request->requestId = nextFreeId;
    m_requests.append(request);
    QMetaObject::invokeMethod(this, "handleRequests", Qt::QueuedConnection);
    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Succeeded);
}

void Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::requestFinished(quint64 requestId, const QList<QVariant> &outParams)
{
    QList<Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData*>::iterator it = m_requests.begin();
    while (it != m_requests.end()) {
        if ((*it)->requestId == requestId) {
            (*it)->status = Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestFinished;
            (*it)->outParams = outParams;
            QMetaObject::invokeMethod(this, "handleRequests", Qt::QueuedConnection);
            return;
        }
        it++;
    }

    qCWarning(lcSailfishSecretsDaemon) << "Unable to finish unknown request:" << requestId;
}

void Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::handleRequests()
{
    qCDebug(lcSailfishSecretsDaemon) << "have:" << m_requests.size() << "in queue.";
    QElapsedTimer yieldTimer;
    yieldTimer.start();
    bool completed = false;
    QList<Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData*>::iterator it = m_requests.begin();
    while (it != m_requests.end()) {
        Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData *request = *it;
        completed = false;
        if (request->status == RequestPending) {
            // This is a new request we haven't seen before.
            // Track the peer connection (if we haven't already), and then handle the request.
            //trackPeerConnection(request); // TODO: is this needed?
            request->status = RequestInProgress;
            handlePendingRequest(request, &completed);
            if (completed) {
                it = m_requests.erase(it);
                delete request;
            } else {
                it++;
            }
        } else if (request->status == RequestFinished) {
            // This (asynchronous) request is in Finished state.  We need to send the response.
            handleFinishedRequest(request, &completed);
            if (completed) {
                it = m_requests.erase(it);
                delete request;
            } else {
                it++;
            }
        } else if (request->status == RequestInProgress) {
            // this request is already in progress.
            it++;
        }

        if (m_requests.size() && yieldTimer.elapsed() > 100) {
            // If we've taken more than 100 msec to handle requests, then we should
            // yield to the event loop after queuing up another handleRequests event.
            // This ensures that we stay responsive to DBus requests even if we have
            // a large number of incoming client requests to handle.
            QMetaObject::invokeMethod(this, "handleRequests", Qt::QueuedConnection);
            break;
        }
    }

    // no more pending requests to handle, or yielding to event loop.
    qint64 nsecs = yieldTimer.nsecsElapsed(), msecs = 0, secs = 0;
    while (nsecs > 1000000) {
        nsecs -= 1000000;
        msecs += 1;
    }
    while (msecs > 1000) {
        msecs -= 1000;
        secs += 1;
    }
    qCDebug(lcSailfishSecretsDaemon) << "Yielding to event loop with:" << m_requests.size() << "requests still in queue after"
                                       << secs << "seconds," << msecs << "milliseconds," << nsecs << "nanoseconds of processing.";
}
