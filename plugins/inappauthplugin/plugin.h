/*
 * Copyright (C) 2017 Jolla Ltd.
 * Contact: Chris Adams <chris.adams@jollamobile.com>
 * All rights reserved.
 * BSD 3-Clause License, see LICENSE.
 */

#ifndef SAILFISHSECRETS_PLUGIN_AUTHENTICATION_INAPP_H
#define SAILFISHSECRETS_PLUGIN_AUTHENTICATION_INAPP_H

#include "Secrets/extensionplugins.h"
#include "Secrets/result.h"
#include "Secrets/uirequest.h"
#include "Secrets/uirequestwatcher.h"

#include <QObject>
#include <QVector>
#include <QString>
#include <QByteArray>
#include <QCryptographicHash>
#include <QMutexLocker>
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(lcSailfishSecretsPluginSqlite)

namespace Sailfish {

namespace Secrets {

namespace Daemon {

namespace Plugins {

class Q_DECL_EXPORT InAppPlugin : public Sailfish::Secrets::AuthenticationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID Sailfish_Secrets_AuthenticationPlugin_IID)
    Q_INTERFACES(Sailfish::Secrets::AuthenticationPlugin)

public:
    InAppPlugin(QObject *parent = Q_NULLPTR);
    ~InAppPlugin();

    bool isTestPlugin() const Q_DECL_OVERRIDE {
#ifdef SAILFISH_SECRETS_BUILD_TEST_PLUGIN
        return true;
#else
        return false;
#endif
    }

    QString name() const Q_DECL_OVERRIDE { return QLatin1String("org.sailfishos.secrets.plugin.authentication.inapp"); }
    Sailfish::Secrets::AuthenticationPlugin::AuthenticationType authenticationType() const Q_DECL_OVERRIDE { return Sailfish::Secrets::AuthenticationPlugin::ApplicationSpecificAuthentication; }

    Sailfish::Secrets::Result beginAuthentication(
                uint callerPid,
                qint64 requestId,
                const QString &callerApplicationId,
                const QString &collectionName,
                const QString &secretName,
                const QString &uiServiceAddress) Q_DECL_OVERRIDE;

private Q_SLOTS:
    void uiRequestFinished(quint64 requestId);
    void uiRequestResponse(quint64 requestId,
                           const Sailfish::Secrets::Result &result,
                           const Sailfish::Secrets::UiResponse &response);

private:
    QMap<quint64, Sailfish::Secrets::UiRequestWatcher *> m_requests;
};

} // namespace Plugins

} // namespace Daemon

} // namespace Secrets

} // namespace Sailfish

#endif // SAILFISHSECRETS_PLUGIN_AUTHENTICATION_INAPP_H
