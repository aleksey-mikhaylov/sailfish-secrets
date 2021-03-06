/*
 * Copyright (C) 2017 Jolla Ltd.
 * Contact: Chris Adams <chris.adams@jollamobile.com>
 * All rights reserved.
 * BSD 3-Clause License, see LICENSE.
 */

#ifndef SAILFISHSECRETS_APIIMPL_SECRETS_P_H
#define SAILFISHSECRETS_APIIMPL_SECRETS_P_H

#include "requestqueue_p.h"
#include "applicationpermissions_p.h"
#include "secretsdatabase_p.h"

#include "Secrets/extensionplugins.h"
#include "Secrets/secretmanager.h"
#include "Secrets/result.h"

#include "Crypto/result.h"
#include "Crypto/key.h"

#include <QtDBus/QDBusContext>

namespace Sailfish {

namespace Secrets {

namespace Daemon {

namespace ApiImpl {

class SecretsRequestQueue;
class SecretsDBusObject : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.sailfishos.secrets")
    Q_CLASSINFO("D-Bus Introspection", ""
    "  <interface name=\"org.sailfishos.secrets\">\n"
    "      <method name=\"getPluginInfo\">\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <arg name=\"storagePlugins\" type=\"(si)\" direction=\"out\" />\n"
    "          <arg name=\"encryptionPlugins\" type=\"(sii)\" direction=\"out\" />\n"
    "          <arg name=\"encryptedStoragePlugins\" type=\"(siii)\" direction=\"out\" />\n"
    "          <arg name=\"authenticationPlugins\" type=\"(si)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out1\" value=\"QVector<Sailfish::Secrets::StoragePluginInfo>\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out2\" value=\"QVector<Sailfish::Secrets::EncryptionPluginInfo>\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out3\" value=\"QVector<Sailfish::Secrets::EncryptedStoragePluginInfo>\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out4\" value=\"QVector<Sailfish::Secrets::AuthenticationPluginInfo>\" />\n"
    "      </method>\n"
    "      <method name=\"createCollection\">\n"
    "          <arg name=\"collectionName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"storagePluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"encryptionPluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"unlockSemantic\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"accessControlMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In3\" value=\"Sailfish::Secrets::SecretManager::DeviceLockUnlockSemantic\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In4\" value=\"Sailfish::Secrets::SecretManager::AccessControlMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"createCollection\">\n"
    "          <arg name=\"collectionName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"storagePluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"encryptionPluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"authenticationPluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"unlockSemantic\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"customLockTimeoutMs\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"accessControlMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"uiServiceAddress\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In4\" value=\"Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In6\" value=\"Sailfish::Secrets::SecretManager::AccessControlMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In7\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"deleteCollection\">\n"
    "          <arg name=\"collectionName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"uiServiceAddress\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In1\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"setSecret\">\n"
    "          <arg name=\"collectionName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"secretName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"secret\" type=\"ay\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"uiServiceAddress\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In3\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"setSecret\">\n"
    "          <arg name=\"storagePluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"encryptionPluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"secretName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"secret\" type=\"ay\" direction=\"in\" />\n"
    "          <arg name=\"unlockSemantic\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"accessControlMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In4\" value=\"Sailfish::Secrets::SecretManager::DeviceLockUnlockSemantic\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In5\" value=\"Sailfish::Secrets::SecretManager::AccessControlMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In6\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"setSecret\">\n"
    "          <arg name=\"storagePluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"encryptionPluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"authenticationPluginName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"secretName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"secret\" type=\"ay\" direction=\"in\" />\n"
    "          <arg name=\"unlockSemantic\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"customLockTimeoutMs\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"accessControlMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"uiServiceAddress\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In5\" value=\"Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In7\" value=\"Sailfish::Secrets::SecretManager::AccessControlMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In8\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"getSecret\">\n"
    "          <arg name=\"collectionName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"secretName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"uiServiceAddress\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <arg name=\"secret\" type=\"ay\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In2\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"getSecret\">\n"
    "          <arg name=\"secretName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"uiServiceAddress\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <arg name=\"secret\" type=\"ay\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In1\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"deleteSecret\">\n"
    "          <arg name=\"collectionName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"secretName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"uiServiceAddress\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In2\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "      <method name=\"deleteSecret\">\n"
    "          <arg name=\"secretName\" type=\"s\" direction=\"in\" />\n"
    "          <arg name=\"userInteractionMode\" type=\"i\" direction=\"in\" />\n"
    "          <arg name=\"result\" type=\"(iis)\" direction=\"out\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.In1\" value=\"Sailfish::Secrets::SecretManager::UserInteractionMode\" />\n"
    "          <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"Sailfish::Secrets::Result\" />\n"
    "      </method>\n"
    "  </interface>\n"
    "")

public:
    SecretsDBusObject(Sailfish::Secrets::Daemon::ApiImpl::SecretsRequestQueue *parent);

public Q_SLOTS:
    // retrieve information about available plugins
    void getPluginInfo(
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result,
            QVector<Sailfish::Secrets::StoragePluginInfo> &storagePlugins,
            QVector<Sailfish::Secrets::EncryptionPluginInfo> &encryptionPlugins,
            QVector<Sailfish::Secrets::EncryptedStoragePluginInfo> &encryptedStoragePlugins,
            QVector<Sailfish::Secrets::AuthenticationPluginInfo> &authenticationPlugins);

    // create a DeviceLock-protected collection
    void createCollection(
            const QString &collectionName,
            const QString &storagePluginName,
            const QString &encryptionPluginName,
            Sailfish::Secrets::SecretManager::DeviceLockUnlockSemantic unlockSemantic,
            Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result);

    // create a CustomLock-protected collection
    void createCollection(
            const QString &collectionName,
            const QString &storagePluginName,
            const QString &encryptionPluginName,
            const QString &authenticationPluginName,
            Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic unlockSemantic,
            int customLockTimeoutMs,
            Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QString &uiServiceAddress,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result);

    // delete a collection
    void deleteCollection(
            const QString &collectionName,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result);

    // set a secret in a collection
    void setSecret(
            const QString &collectionName,
            const QString &secretName,
            const QByteArray &secret,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QString &uiServiceAddress,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result);

    // set a standalone DeviceLock-protected secret
    void setSecret(
            const QString &storagePluginName,
            const QString &encryptionPluginName,
            const QString &secretName,
            const QByteArray &secret,
            Sailfish::Secrets::SecretManager::DeviceLockUnlockSemantic unlockSemantic,
            Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result);

    // set a standalone CustomLock-protected secret
    void setSecret(
            const QString &storagePluginName,
            const QString &encryptionPluginName,
            const QString &authenticationPluginName,
            const QString &secretName,
            const QByteArray &secret,
            Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic unlockSemantic,
            int customLockTimeoutMs,
            Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QString &uiServiceAddress,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result);

    // get a secret in a collection
    void getSecret(
            const QString &collectionName,
            const QString &secretName,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QString &uiServiceAddress,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result,
            QByteArray &secret);

    // get a standalone secret
    void getSecret(
            const QString &secretName,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QString &uiServiceAddress,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result,
            QByteArray &secret);

    // delete a secret in a collection
    void deleteSecret(
            const QString &collectionName,
            const QString &secretName,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QString &uiServiceAddress,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result);

    // delete a standalone secret
    void deleteSecret(
            const QString &secretName,
            Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
            const QDBusMessage &message,
            Sailfish::Secrets::Result &result);

private:
    Sailfish::Secrets::Daemon::ApiImpl::SecretsRequestQueue *m_requestQueue;
};

class RequestProcessor;
class SecretsRequestQueue : public Sailfish::Secrets::Daemon::ApiImpl::RequestQueue
{
    Q_OBJECT

public:
    SecretsRequestQueue(Sailfish::Secrets::Daemon::Controller *parent, const QString &pluginDir, bool autotestMode);
    ~SecretsRequestQueue();

    void handlePendingRequest(Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData *request, bool *completed) Q_DECL_OVERRIDE;
    void handleFinishedRequest(Sailfish::Secrets::Daemon::ApiImpl::RequestQueue::RequestData *request, bool *completed) Q_DECL_OVERRIDE;
    QString requestTypeToString(int type) const Q_DECL_OVERRIDE;

private:
    Sailfish::Secrets::Daemon::ApiImpl::Database m_db;
    Sailfish::Secrets::Daemon::ApiImpl::ApplicationPermissions *m_appPermissions;
    Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor *m_requestProcessor;

public: // Crypto API helper methods.
    // these methods are provided in order to implement Crypto functionality
    // while using just one single database (for atomicity etc).
    void asynchronousCryptoRequestCompleted(quint64 cryptoRequestId, const Sailfish::Secrets::Result &result, const QVariantList &parameters);
    // the first methods are synchronous:
    Sailfish::Secrets::Result storagePluginNames(pid_t callerPid, quint64 cryptoRequestId, QStringList *names) const;
    Sailfish::Secrets::Result keyEntryIdentifiers(pid_t callerPid, quint64 cryptoRequestId, QVector<Sailfish::Crypto::Key::Identifier> *identifiers);
    Sailfish::Secrets::Result keyEntry(pid_t callerPid, quint64 cryptoRequestId, const Sailfish::Crypto::Key::Identifier &identifier, QString *cryptoPluginName, QString *storagePluginName);
    Sailfish::Secrets::Result addKeyEntry(pid_t callerPid, quint64 cryptoRequestId, const Sailfish::Crypto::Key::Identifier &identifier, const QString &cryptoPluginName, const QString &storagePluginName);
    Sailfish::Secrets::Result removeKeyEntry(pid_t callerPid, quint64 cryptoRequestId, const Sailfish::Crypto::Key::Identifier &identifier);
    // the others are possibly-asynchronous methods:
    Sailfish::Secrets::Result storedKey(pid_t callerPid, quint64 cryptoRequestId, const Sailfish::Crypto::Key::Identifier &identifier, QByteArray *serialisedKey);
    Sailfish::Secrets::Result storeKey(pid_t callerPid, quint64 cryptoRequestId, const Sailfish::Crypto::Key::Identifier &identifier, const QByteArray &serialisedKey, const QString &storagePluginName);
    Sailfish::Secrets::Result deleteStoredKey(pid_t callerPid, quint64 cryptoRequestId, const Sailfish::Crypto::Key::Identifier &identifier);
Q_SIGNALS:
    void storedKeyCompleted(quint64 cryptoRequestId, const Sailfish::Secrets::Result &result, const QByteArray &serialisedKey);
    void storeKeyCompleted(quint64 cryptoRequestId, const Sailfish::Secrets::Result &result);
    void deleteStoredKeyCompleted(quint64 cryptoRequestId, const Sailfish::Secrets::Result &result);
private:
    enum CryptoApiHelperRequestType {
        InvalidCryptoApiHelperRequest = 0,
        StoragePluginNamesCryptoApiHelperRequest,
        KeyEntryIdentifiers,
        KeyEntryCryptoApiHelperRequest,
        AddKeyEntryCryptoApiHelperRequest,
        RemoveKeyEntryCryptoApiHelperRequest,
        StoredKeyCryptoApiHelperRequest,
        DeleteStoredKeyCryptoApiHelperRequest,
        StoreKeyCryptoApiHelperRequest
    };
    QMap<quint64, CryptoApiHelperRequestType> m_cryptoApiHelperRequests; // crypto request id to crypto api call type.
};

enum RequestType {
    InvalidRequest = 0,
    GetPluginInfoRequest,
    CreateDeviceLockCollectionRequest,
    CreateCustomLockCollectionRequest,
    DeleteCollectionRequest,
    SetCollectionSecretRequest,
    SetStandaloneDeviceLockSecretRequest,
    SetStandaloneCustomLockSecretRequest,
    GetCollectionSecretRequest,
    GetStandaloneSecretRequest,
    DeleteCollectionSecretRequest,
    DeleteStandaloneSecretRequest
};

} // ApiImpl

} // Daemon

} // Secrets

} // Sailfish

#endif // SAILFISHSECRETS_APIIMPL_SECRETS_P_H
