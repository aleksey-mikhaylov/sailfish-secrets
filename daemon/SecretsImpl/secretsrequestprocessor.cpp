/*
 * Copyright (C) 2017 Jolla Ltd.
 * Contact: Chris Adams <chris.adams@jollamobile.com>
 * All rights reserved.
 * BSD 3-Clause License, see LICENSE.
 */

#include "secretsrequestprocessor_p.h"
#include "applicationpermissions_p.h"
#include "logging_p.h"

#include "Secrets/result.h"
#include "Secrets/secretmanager.h"
#include "Secrets/secret.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QPluginLoader>
#include <QtCore/QDataStream>
#include <QtCore/QVariant>
#include <QtCore/QString>
#include <QtCore/QList>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QDir>

// In real system, we would generate a secure key on first boot,
// and store it via a hardware-supported secure storage mechanism.
// If we ever update the secure key, we would need to decrypt all
// values stored in the secrets database with the old key, encrypt
// them with the new key, and write the updated values back to storage.
static const QByteArray SystemEncryptionKey = QByteArray("example_encryption_key");
// In real system, we would store the device lock key (hash) somewhere
// securely.  We use this device lock key to lock/unlock device-lock
// protected collections.
static const QByteArray DeviceLockKey = QByteArray("example_device_lock_key");

namespace {
    QByteArray rehashHash(const QByteArray &hash) {
        QCryptographicHash rehash(QCryptographicHash::QCryptographicHash::Sha3_256);
        rehash.addData(hash);
        return rehash.result();
    }

    QString generateHashedSecretName(const QString &collectionName, const QString &secretName) {
        QCryptographicHash keyHash(QCryptographicHash::QCryptographicHash::Sha3_256);
        QByteArray data = collectionName.toUtf8() + secretName.toUtf8();
        keyHash.addData(data);
        QByteArray hashed = keyHash.result();

        // do PBKDF style repeated hashing
        for (int i = 0; i < 100; ++i) {
            hashed = rehashHash(hashed);
        }

        return QString::fromLatin1(hashed.toBase64());
    }
}

Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::DatabaseLocker::~DatabaseLocker()
{
    if (mutex()) {
        // The database was not already within a transaction when we were constructed
        // and thus should not be in a transaction when we destruct.
        // That is, check that the beginTransaction()/commitTransaction()/rollbackTransaction()
        // calls are balanced within a given locker scope.
        if (m_db->withinTransaction()) {
            qCWarning(lcSailfishSecretsDaemonDatabase) << "Locker: transaction not balanced!  None -> Within!";
        }
    } else {
        // The database was already within a transaction when we were constructed
        // and thus should still be in that transaction when we destruct.
        if (!m_db->withinTransaction()) {
            if (m_db->withinTransaction()) {
                qCWarning(lcSailfishSecretsDaemonDatabase) << "Locker: transaction not balanced!  Within -> None!";
            }
        }
    }
}

Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::RequestProcessor(Sailfish::Secrets::Daemon::ApiImpl::Database *db,
                 Sailfish::Secrets::Daemon::ApiImpl::ApplicationPermissions *appPermissions,
                 Sailfish::Secrets::Daemon::ApiImpl::SecretsRequestQueue *parent)
    : QObject(parent), m_requestQueue(parent), m_db(db), m_appPermissions(appPermissions)
{
    // Add the "standalone" collection.
    // Note that it is a "notional" collection,
    // existing only to satisfy the database constraints.
    const QString insertCollectionQuery = QStringLiteral(
                "INSERT INTO Collections ("
                  "CollectionName,"
                  "ApplicationId,"
                  "UsesDeviceLockKey,"
                  "StoragePluginName,"
                  "EncryptionPluginName,"
                  "AuthenticationPluginName,"
                  "UnlockSemantic,"
                  "CustomLockTimeoutMs,"
                  "AccessControlMode"
                ")"
                " VALUES ("
                  "?,?,?,?,?,?,?,?,?"
                ");");

    QString errorText;
    Database::Query iq = m_db->prepare(insertCollectionQuery, &errorText);

    QVariantList ivalues;
    ivalues << QVariant::fromValue<QString>(QLatin1String("standalone"))
            << QVariant::fromValue<QString>(QLatin1String("standalone"))
            << 0
            << QVariant::fromValue<QString>(QLatin1String("standalone"))
            << QVariant::fromValue<QString>(QLatin1String("standalone"))
            << QVariant::fromValue<QString>(QLatin1String("standalone"))
            << 0
            << 0
            << 0;
    iq.bindValues(ivalues);

    if (m_db->beginTransaction()) {
        if (m_db->execute(iq, &errorText)) {
            m_db->commitTransaction();
        } else {
            m_db->rollbackTransaction();
        }
    }
}

bool
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::loadPlugins(const QString &pluginDir, bool autotestMode)
{
    qCDebug(lcSailfishSecretsDaemon) << "Loading plugins from directory:" << pluginDir;
    QDir dir(pluginDir);
    Q_FOREACH (const QString &pluginFile, dir.entryList(QDir::Files | QDir::NoDot | QDir::NoDotDot, QDir::Name)) {
        // load the plugin and query it for its data.
        QPluginLoader loader(pluginFile);
        QObject *plugin = loader.instance();
        Sailfish::Secrets::StoragePlugin *storagePlugin = qobject_cast<Sailfish::Secrets::StoragePlugin*>(plugin);
        Sailfish::Secrets::EncryptionPlugin *encryptionPlugin = qobject_cast<Sailfish::Secrets::EncryptionPlugin*>(plugin);
        Sailfish::Secrets::EncryptedStoragePlugin *encryptedStoragePlugin = qobject_cast<Sailfish::Secrets::EncryptedStoragePlugin*>(plugin);
        Sailfish::Secrets::AuthenticationPlugin *authenticationPlugin = qobject_cast<Sailfish::Secrets::AuthenticationPlugin*>(plugin);
        if (storagePlugin) {
            if (storagePlugin->isTestPlugin() != autotestMode) {
                qCDebug(lcSailfishSecretsDaemon) << "ignoring storage plugin:" << pluginFile << "due to mode";
                loader.unload();
                continue;
            } else if (storagePlugin->name().isEmpty() || m_storagePlugins.contains(storagePlugin->name())) {
                qCDebug(lcSailfishSecretsDaemon) << "ignoring storage plugin:" << pluginFile << "with duplicate name:" << storagePlugin->name();
                loader.unload();
                continue;
            } else {
                qCDebug(lcSailfishSecretsDaemon) << "loading storage plugin:" << pluginFile << "with name:" << storagePlugin->name();
                m_storagePlugins.insert(storagePlugin->name(), storagePlugin);
            }
        } else if (encryptionPlugin) {
            if (encryptionPlugin->isTestPlugin() != autotestMode) {
                qCDebug(lcSailfishSecretsDaemon) << "ignoring encryption plugin:" << pluginFile << "due to mode";
                loader.unload();
                continue;
            } else if (encryptionPlugin->name().isEmpty() || m_storagePlugins.contains(encryptionPlugin->name())) {
                qCDebug(lcSailfishSecretsDaemon) << "ignoring encryption plugin:" << pluginFile << "with duplicate name:" << encryptionPlugin->name();
                loader.unload();
                continue;
            } else {
                qCDebug(lcSailfishSecretsDaemon) << "loading encryption plugin:" << pluginFile << "with name:" << encryptionPlugin->name();
                m_encryptionPlugins.insert(encryptionPlugin->name(), encryptionPlugin);
            }
        } else if (encryptedStoragePlugin) {
            if (encryptedStoragePlugin->isTestPlugin() != autotestMode) {
                qCDebug(lcSailfishSecretsDaemon) << "ignoring encrypted storage plugin:" << pluginFile << "due to mode";
                loader.unload();
                continue;
            } else if (encryptedStoragePlugin->name().isEmpty() || m_encryptedStoragePlugins.contains(encryptedStoragePlugin->name())) {
                qCDebug(lcSailfishSecretsDaemon) << "ignoring encrypted storage plugin:" << pluginFile << "with duplicate name:" << encryptedStoragePlugin->name();
                loader.unload();
                continue;
            } else {
                qCDebug(lcSailfishSecretsDaemon) << "loading encrypted storage plugin:" << pluginFile << "with name:" << encryptedStoragePlugin->name();
                m_encryptedStoragePlugins.insert(encryptedStoragePlugin->name(), encryptedStoragePlugin);
            }
        } else if (authenticationPlugin) {
            if (authenticationPlugin->isTestPlugin() != autotestMode) {
                qCDebug(lcSailfishSecretsDaemon) << "ignoring authentication plugin:" << pluginFile << "due to mode";
                loader.unload();
                continue;
            } else if (authenticationPlugin->name().isEmpty() || m_authenticationPlugins.contains(authenticationPlugin->name())) {
                qCDebug(lcSailfishSecretsDaemon) << "ignoring authentication plugin:" << pluginFile << "with duplicate name:" << authenticationPlugin->name();
                loader.unload();
                continue;
            } else {
                qCDebug(lcSailfishSecretsDaemon) << "loading authentication plugin:" << pluginFile << "with name:" << authenticationPlugin->name();
                m_authenticationPlugins.insert(authenticationPlugin->name(), authenticationPlugin);
                connect(authenticationPlugin, &Sailfish::Secrets::AuthenticationPlugin::authenticationCompleted,
                        this, &Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::authenticationCompleted);
            }
        } else {
            qCWarning(lcSailfishSecretsDaemon) << "ignoring plugin:" << pluginFile << "- not a secrets plugin or Qt version mismatch";
            loader.unload();
            continue;
        }
    }

    return true;
}

// retrieve information about available plugins
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::getPluginInfo(
        pid_t callerPid,
        quint64 requestId,
        QVector<Sailfish::Secrets::StoragePluginInfo> *storagePlugins,
        QVector<Sailfish::Secrets::EncryptionPluginInfo> *encryptionPlugins,
        QVector<Sailfish::Secrets::EncryptedStoragePluginInfo> *encryptedStoragePlugins,
        QVector<Sailfish::Secrets::AuthenticationPluginInfo> *authenticationPlugins)
{
    Q_UNUSED(callerPid); // TODO: perform access control request to see if the application has permission to read secure storage metadata.
    Q_UNUSED(requestId); // The request is synchronous, so don't need the requestId.

    for (const Sailfish::Secrets::StoragePlugin *plugin : m_storagePlugins.values()) {
        storagePlugins->append(Sailfish::Secrets::StoragePluginInfo(plugin));
    }
    for (const Sailfish::Secrets::EncryptionPlugin *plugin : m_encryptionPlugins.values()) {
        encryptionPlugins->append(Sailfish::Secrets::EncryptionPluginInfo(plugin));
    }
    for (const Sailfish::Secrets::EncryptedStoragePlugin *plugin : m_encryptedStoragePlugins.values()) {
        encryptedStoragePlugins->append(Sailfish::Secrets::EncryptedStoragePluginInfo(plugin));
    }
    for (const Sailfish::Secrets::AuthenticationPlugin *plugin : m_authenticationPlugins.values()) {
        authenticationPlugins->append(Sailfish::Secrets::AuthenticationPluginInfo(plugin));
    }

    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Succeeded);
}

// create a DeviceLock-protected collection
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::createDeviceLockCollection(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        Sailfish::Secrets::SecretManager::DeviceLockUnlockSemantic unlockSemantic,
        Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode)
{
    Q_UNUSED(requestId); // the request would only be asynchronous if we needed to perform the access control request, so until then it's always synchronous.

    if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Reserved collection name given"));
    } else if (storagePluginName == encryptionPluginName && !m_encryptedStoragePlugins.contains(storagePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(storagePluginName));
    } else if (!m_storagePlugins.contains(storagePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(storagePluginName));
    } else if (!m_encryptionPlugins.contains(encryptionPluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(encryptionPluginName));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    DatabaseLocker locker(m_db);

    // Whenever we modify the master database + perform a plugin operation,
    // we should ensure that we do it in such an order that only the MASTER
    // database can be "wrong", as we can correct that.
    // So, in this case, we:
    // 1) start transaction
    // 2) check that the collection name doesn't already exist, else fail
    // 3) insert the new collection entry into the master Collections table
    // 4) commit the transaction
    // 5) tell the storage plugin to create the new collection
    // 6) if (5) failed, start new transaction to remove the collection, commit.
    // In the future, we should mark the row as "dirty" via in-memory flag, if (6) fails,
    // so that we can re-attempt to remove it, at a later point in time.

    const QString selectCollectionsCountQuery = QStringLiteral(
                 "SELECT"
                    " Count(*)"
                  " FROM Collections"
                  " WHERE CollectionName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectCollectionsCountQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select collections query: %1").arg(errorText));
    }

    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select collections query: %1").arg(errorText));
    }

    if (sq.next() && sq.value(0).value<int>() > 0) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionAlreadyExistsError,
                                         QString::fromLatin1("Collection already exists: %1").arg(collectionName));
    }

    const QString insertCollectionQuery = QStringLiteral(
                "INSERT INTO Collections ("
                  "CollectionName,"
                  "ApplicationId,"
                  "UsesDeviceLockKey,"
                  "StoragePluginName,"
                  "EncryptionPluginName,"
                  "AuthenticationPluginName,"
                  "UnlockSemantic,"
                  "CustomLockTimeoutMs,"
                  "AccessControlMode"
                ")"
                " VALUES ("
                  "?,?,1,?,?,?,?,0,?"
                ");");

    Database::Query iq = m_db->prepare(insertCollectionQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare insert collection query: %1").arg(errorText));
    }

    QVariantList ivalues;
    ivalues << QVariant::fromValue<QString>(collectionName)
            << callerApplicationId
            << storagePluginName
            << encryptionPluginName
            << Sailfish::Secrets::SecretManager::DefaultAuthenticationPluginName
            << static_cast<int>(unlockSemantic)
            << static_cast<int>(accessControlMode);
    iq.bindValues(ivalues);

    if (!m_db->beginTransaction()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to begin insert collection transaction"));
    }

    if (!m_db->execute(iq, &errorText)) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute insert collection query: %1").arg(errorText));
    }

    if (!m_db->commitTransaction()) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to commit insert collection transaction"));
    }

    Sailfish::Secrets::Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->createCollection(collectionName, DeviceLockKey);
    } else {
        pluginResult = m_storagePlugins[storagePluginName]->createCollection(collectionName);
        m_collectionAuthenticationKeys.insert(collectionName, DeviceLockKey);
    }

    if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
        // The plugin was unable to create the collection in its storage.  Let's delete it from our master table.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->createCollection() call succeeds,
        // but the master table commit fails.
        const QString deleteCollectionQuery = QStringLiteral(
                    "DELETE FROM Collections"
                    " WHERE CollectionName = ?;");

        Database::Query dq = m_db->prepare(deleteCollectionQuery, &errorText);
        if (!errorText.isEmpty()) {
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to prepare delete collection query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        QVariantList values;
        values << QVariant::fromValue<QString>(collectionName);
        dq.bindValues(values);

        if (!m_db->beginTransaction()) {
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to begin delete collection transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->execute(dq, &errorText)) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to execute delete collection query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->commitTransaction()) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to commit delete collection transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }
    }

    if (accessControlMode == Sailfish::Secrets::SecretManager::SystemAccessControlMode) {
        // TODO: tell AccessControl daemon to add this datum from its database.
    }

    return pluginResult;
}

// create a CustomLock-protected collection
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::createCustomLockCollection(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &authenticationPluginName,
        Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic unlockSemantic,
        int customLockTimeoutMs,
        Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress)
{
    Q_UNUSED(requestId); // the request would only be asynchronous if we needed to perform the access control request, so until then it's always synchronous.

    if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Reserved collection name given"));
    } else if (storagePluginName == encryptionPluginName && !m_encryptedStoragePlugins.contains(storagePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(storagePluginName));
    } else if (!m_storagePlugins.contains(storagePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(storagePluginName));
    } else if (!m_encryptionPlugins.contains(encryptionPluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(encryptionPluginName));
    } else if (!m_authenticationPlugins.contains(authenticationPluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such authentication plugin exists: %1").arg(authenticationPluginName));
    } else if (m_authenticationPlugins[authenticationPluginName]->authenticationType() == Sailfish::Secrets::AuthenticationPlugin::ApplicationSpecificAuthentication
               && (userInteractionMode != Sailfish::Secrets::SecretManager::InProcessUserInteractionMode || uiServiceAddress.isEmpty())) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresInProcessUserInteraction,
                                         QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(authenticationPluginName));
    } else if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                         QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(authenticationPluginName));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    DatabaseLocker locker(m_db);

    const QString selectCollectionsCountQuery = QStringLiteral(
                 "SELECT"
                    " Count(*)"
                  " FROM Collections"
                  " WHERE CollectionName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectCollectionsCountQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select collections query: %1").arg(errorText));
    }

    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select collections query: %1").arg(errorText));
    }

    bool found = false;
    if (sq.next()) {
        found = sq.value(0).value<int>() > 0;
    }

    if (found) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionAlreadyExistsError,
                                         QString::fromLatin1("Collection already exists: %1").arg(collectionName));
    }

    // perform the authentication required to get the authentication key which will be used
    // to encrypt the data in this collection.
    Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[authenticationPluginName]->beginAuthentication(
                callerPid,
                requestId,
                callerApplicationId,
                collectionName,
                QString(),
                uiServiceAddress);
    if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
        return authenticationResult;
    }

    m_pendingRequests.insert(requestId,
                             Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Sailfish::Secrets::Daemon::ApiImpl::CreateCustomLockCollectionRequest,
                                 QVariantList() << collectionName
                                                << storagePluginName
                                                << encryptionPluginName
                                                << authenticationPluginName
                                                << unlockSemantic
                                                << customLockTimeoutMs
                                                << accessControlMode
                                                << userInteractionMode
                                                << uiServiceAddress));
    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
}

Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::createCustomLockCollectionWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &authenticationPluginName,
        Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic unlockSemantic,
        int customLockTimeoutMs,
        Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress,
        const QByteArray &authenticationKey)
{
    // may be required for access control requests in the future
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(uiServiceAddress);

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // Whenever we modify the master database + perform a plugin operation,
    // we should ensure that we do it in such an order that only the MASTER
    // database can be "wrong", as we can correct that.
    // So, in this case, we:
    // 1) start transaction
    // 2) check that the collection name doesn't already exist, else fail
    // 3) insert the new collection entry into the master Collections table
    // 4) commit the transaction
    // 5) tell the storage plugin to create the new collection
    // 6) if (5) failed, start new transaction to remove the collection, commit.
    // In the future, we should mark the row as "dirty" via in-memory flag, if (6) fails,
    // so that we can re-attempt to remove it, at a later point in time.

    DatabaseLocker locker(m_db);

    const QString selectCollectionsCountQuery = QStringLiteral(
                 "SELECT"
                    " Count(*)"
                  " FROM Collections"
                  " WHERE CollectionName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectCollectionsCountQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select collections query: %1").arg(errorText));
    }

    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select collections query: %1").arg(errorText));
    }

    // check again, in case something else added it while we were handling the asynchronous UI flow.
    bool found = false;
    if (sq.next()) {
        found = sq.value(0).value<int>() > 0;
    }

    if (found) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionAlreadyExistsError,
                                         QString::fromLatin1("Collection already exists: %1").arg(collectionName));
    }

    const QString insertCollectionQuery = QStringLiteral(
                "INSERT INTO Collections ("
                  "CollectionName,"
                  "ApplicationId,"
                  "UsesDeviceLockKey,"
                  "StoragePluginName,"
                  "EncryptionPluginName,"
                  "AuthenticationPluginName,"
                  "UnlockSemantic,"
                  "CustomLockTimeoutMs,"
                  "AccessControlMode"
                ")"
                " VALUES ("
                  "?,?,0,?,?,?,?,?,?"
                ");");

    Database::Query iq = m_db->prepare(insertCollectionQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare insert collection query: %1").arg(errorText));
    }

    QVariantList ivalues;
    ivalues << QVariant::fromValue<QString>(collectionName)
            << callerApplicationId
            << storagePluginName
            << encryptionPluginName
            << authenticationPluginName
            << static_cast<int>(unlockSemantic)
            << customLockTimeoutMs
            << static_cast<int>(accessControlMode);
    iq.bindValues(ivalues);

    if (!m_db->beginTransaction()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to begin insert collection transaction"));
    }

    if (!m_db->execute(iq, &errorText)) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute insert collection query: %1").arg(errorText));
    }

    if (!m_db->commitTransaction()) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to commit insert collection transaction"));
    }

    Sailfish::Secrets::Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->createCollection(collectionName, authenticationKey);
    } else {
        pluginResult = m_storagePlugins[storagePluginName]->createCollection(collectionName);
        m_collectionAuthenticationKeys.insert(collectionName, authenticationKey);
        // TODO: also set CustomLockTimeoutMs, flag for "is custom key", etc.
    }

    if (pluginResult.code() == Sailfish::Secrets::Result::Failed) {
        // The plugin was unable to create the collection in its storage.  Let's delete it from our master table.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->createCollection() call succeeds,
        // but the master table commit fails.
        const QString deleteCollectionQuery = QStringLiteral(
                    "DELETE FROM Collections"
                    " WHERE CollectionName = ?;");

        Database::Query dq = m_db->prepare(deleteCollectionQuery, &errorText);
        if (!errorText.isEmpty()) {
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to prepare delete collection query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        QVariantList values;
        values << QVariant::fromValue<QString>(collectionName);
        dq.bindValues(values);

        if (!m_db->beginTransaction()) {
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to begin delete collection transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->execute(dq, &errorText)) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to execute delete collection query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->commitTransaction()) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to commit delete collection transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }
    }

    if (accessControlMode == Sailfish::Secrets::SecretManager::SystemAccessControlMode) {
        // TODO: tell AccessControl daemon to add this datum from its database.
    }

    return pluginResult;
}

// delete a collection
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::deleteCollection(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode)
{
    Q_UNUSED(requestId); // the request would only be asynchronous if we needed to perform the access control request, so until we implement that it's always synchronous.
    Q_UNUSED(userInteractionMode); // ditto ^

    if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Reserved collection name given"));
    } else if (collectionName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Empty collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to delete the collection.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    DatabaseLocker locker(m_db);

    // Whenever we modify the master database + perform a plugin operation,
    // we should ensure that we do it in such an order that only the MASTER
    // database can be "wrong", as we can correct that.
    // So, in this case, we:
    // 1) tell the storage plugin to delete the collection
    // 2) if (1) failed, return immediately
    // 3) start transaction
    // 4) delete the collection entry from the master Collections table
    // 5) commit the transaction
    // In the future, we should mark the row as "dirty" via in-memory flag, if (5) fails,
    // so that we can re-attempt to remove it, at a later point in time.

    const QString selectCollectionsQuery = QStringLiteral(
                 "SELECT"
                    " ApplicationId,"
                    " StoragePluginName,"
                    " EncryptionPluginName,"
                    " AccessControlMode"
                  " FROM Collections"
                  " WHERE CollectionName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectCollectionsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select collections query: %1").arg(errorText));
    }

    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select collections query: %1").arg(errorText));
    }

    bool found = false;
    QString collectionApplicationId;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    Sailfish::Secrets::SecretManager::AccessControlMode collectionAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (sq.next()) {
        found = true;
        collectionApplicationId = sq.value(0).value<QString>();
        collectionStoragePluginName = sq.value(1).value<QString>();
        collectionEncryptionPluginName = sq.value(2).value<QString>();
        collectionAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(sq.value(0).value<int>());
    }

    if (!found) {
        // return success immediately.  No such collection exists, so "deleting" succeeded.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Succeeded);
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName
            && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName.isEmpty() || !m_storagePlugins.contains(collectionStoragePluginName)) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    }

    Sailfish::Secrets::Result pluginResult = collectionStoragePluginName == collectionEncryptionPluginName
            ? m_encryptedStoragePlugins[collectionStoragePluginName]->removeCollection(collectionName)
            : m_storagePlugins[collectionStoragePluginName]->removeCollection(collectionName);
    if (pluginResult.code() == Sailfish::Secrets::Result::Failed) {
        return pluginResult;
    }

    // successfully removed from plugin storage, now remove the entry from the master table.
    m_collectionAuthenticationKeys.remove(collectionName);
    m_collectionLockTimers.remove(collectionName);
    const QString deleteCollectionQuery = QStringLiteral(
                "DELETE FROM Collections"
                " WHERE CollectionName = ?;");

    Database::Query dq = m_db->prepare(deleteCollectionQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare delete collection query: %1").arg(errorText));
    }

    values.clear();
    values << QVariant::fromValue<QString>(collectionName);
    dq.bindValues(values);

    if (!m_db->beginTransaction()) {
        // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to begin delete collection transaction"));
    }

    if (!m_db->execute(dq, &errorText)) {
        m_db->rollbackTransaction();
        // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute delete collection query: %1").arg(errorText));
    }

    if (!m_db->commitTransaction()) {
        m_db->rollbackTransaction();
        // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to commit delete collection transaction"));
    }

    if (collectionAccessControlMode == Sailfish::Secrets::SecretManager::SystemAccessControlMode) {
        // TODO: tell AccessControl daemon to remove this datum from its database.
    }

    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Succeeded);
}

// set a secret in a collection
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::setCollectionSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &secretName,
        const QByteArray &secret,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress)
{
    if (secretName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidSecretError,
                                         QLatin1String("Empty secret name given"));
    } else if (collectionName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Empty collection name given"));
    } else if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Reserved collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    const QString selectCollectionsQuery = QStringLiteral(
                 "SELECT"
                    " ApplicationId,"
                    " UsesDeviceLockKey,"
                    " StoragePluginName,"
                    " EncryptionPluginName,"
                    " AuthenticationPluginName,"
                    " UnlockSemantic,"
                    " CustomLockTimeoutMs,"
                    " AccessControlMode"
                  " FROM Collections"
                  " WHERE CollectionName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectCollectionsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select collections query: %1").arg(errorText));
    }

    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select collections query: %1").arg(errorText));
    }

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    int collectionUnlockSemantic = 0;
    int collectionCustomLockTimeoutMs = 0;
    Sailfish::Secrets::SecretManager::AccessControlMode collectionAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (sq.next()) {
        found = true;
        collectionApplicationId = sq.value(0).value<QString>();
        collectionUsesDeviceLockKey = sq.value(1).value<int>() > 0;
        collectionStoragePluginName = sq.value(2).value<QString>();
        collectionEncryptionPluginName = sq.value(3).value<QString>();
        collectionAuthenticationPluginName = sq.value(4).value<QString>();
        collectionUnlockSemantic = sq.value(5).value<int>();
        collectionCustomLockTimeoutMs = sq.value(6).value<int>();
        collectionAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(sq.value(7).value<int>());
    }

    if (!found) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Nonexistent collection name given"));
    }

    if (collectionAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Collection %1 is owned by a different application").arg(collectionName));
    } else if (collectionStoragePluginName == collectionEncryptionPluginName
            && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionStoragePluginName.isEmpty() || !m_storagePlugins.contains(collectionStoragePluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionEncryptionPluginName.isEmpty() || !m_encryptionPlugins.contains(collectionEncryptionPluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(collectionEncryptionPluginName));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        Sailfish::Secrets::Result pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
            return pluginResult;
        }
        if (!locked) {
            return setCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        collectionName,
                        secretName,
                        secret,
                        userInteractionMode,
                        uiServiceAddress,
                        collectionUsesDeviceLockKey,
                        collectionApplicationId,
                        collectionStoragePluginName,
                        collectionEncryptionPluginName,
                        collectionAuthenticationPluginName,
                        collectionUnlockSemantic,
                        collectionCustomLockTimeoutMs,
                        collectionAccessControlMode,
                        QByteArray());
        }

        if (collectionUsesDeviceLockKey) {
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionIsLockedError,
                                             QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(collectionName));
        }

        if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                             QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
        }

        // perform UI request to get the authentication key for the collection
        Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginAuthentication(
                    callerPid,
                    requestId,
                    callerApplicationId,
                    collectionName,
                    secretName,
                    uiServiceAddress);
        if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
            return authenticationResult;
        }

        m_pendingRequests.insert(requestId,
                                 Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                     callerPid,
                                     requestId,
                                     Sailfish::Secrets::Daemon::ApiImpl::SetCollectionSecretRequest,
                                     QVariantList() << collectionName
                                                    << secretName
                                                    << secret
                                                    << userInteractionMode
                                                    << uiServiceAddress
                                                    << collectionUsesDeviceLockKey
                                                    << collectionApplicationId
                                                    << collectionStoragePluginName
                                                    << collectionEncryptionPluginName
                                                    << collectionAuthenticationPluginName
                                                    << collectionUnlockSemantic
                                                    << collectionCustomLockTimeoutMs
                                                    << collectionAccessControlMode));
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
    }


    if (m_collectionAuthenticationKeys.contains(collectionName)) {
        return setCollectionSecretWithAuthenticationKey(
                    callerPid,
                    requestId,
                    collectionName,
                    secretName,
                    secret,
                    userInteractionMode,
                    uiServiceAddress,
                    collectionUsesDeviceLockKey,
                    collectionApplicationId,
                    collectionStoragePluginName,
                    collectionEncryptionPluginName,
                    collectionAuthenticationPluginName,
                    collectionUnlockSemantic,
                    collectionCustomLockTimeoutMs,
                    collectionAccessControlMode,
                    m_collectionAuthenticationKeys.value(collectionName));
    }

    if (collectionUsesDeviceLockKey) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionIsLockedError,
                                         QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(collectionName));
    }

    if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                         QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
    }

    // perform UI request to get the authentication key for the collection
    Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginAuthentication(
                callerPid,
                requestId,
                callerApplicationId,
                collectionName,
                secretName,
                uiServiceAddress);
    if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
        return authenticationResult;
    }

    m_pendingRequests.insert(requestId,
                             Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Sailfish::Secrets::Daemon::ApiImpl::SetCollectionSecretRequest,
                                 QVariantList() << collectionName
                                                << secretName
                                                << secret
                                                << userInteractionMode
                                                << uiServiceAddress
                                                << collectionUsesDeviceLockKey
                                                << collectionApplicationId
                                                << collectionStoragePluginName
                                                << collectionEncryptionPluginName
                                                << collectionAuthenticationPluginName
                                                << collectionUnlockSemantic
                                                << collectionCustomLockTimeoutMs
                                                << collectionAccessControlMode));
    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
}

Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::setCollectionSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &secretName,
        const QByteArray &secret,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress,
        bool collectionUsesDeviceLockKey,
        const QString &collectionApplicationId,
        const QString &collectionStoragePluginName,
        const QString &collectionEncryptionPluginName,
        const QString &collectionAuthenticationPluginName,
        int collectionUnlockSemantic,
        int collectionCustomLockTimeoutMs,
        Sailfish::Secrets::SecretManager::AccessControlMode collectionAccessControlMode,
        const QByteArray &authenticationKey)
{
    // In the future, we may need these for access control UI flows.
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(uiServiceAddress);

    const QString selectSecretsCountQuery = QStringLiteral(
                 "SELECT"
                    " Count(*)"
                  " FROM Secrets"
                  " WHERE CollectionName = ?"
                  " AND SecretName = ?;"
             );

    QString errorText;
    Database::Query ssq = m_db->prepare(selectSecretsCountQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select secrets query: %1").arg(errorText));
    }

    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);
    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    values << QVariant::fromValue<QString>(hashedSecretName);
    ssq.bindValues(values);

    if (!m_db->execute(ssq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select secrets query: %1").arg(errorText));
    }

    bool secretAlreadyExists = false;
    if (ssq.next()) {
        secretAlreadyExists = ssq.value(0).value<int>() > 0;
    }

    if (!secretAlreadyExists) {
        // Write to the master database prior to the storage plugin.
        const QString insertSecretQuery = QStringLiteral(
                    "INSERT INTO Secrets ("
                      "CollectionName,"
                      "SecretName,"
                      "ApplicationId,"
                      "UsesDeviceLockKey,"
                      "StoragePluginName,"
                      "EncryptionPluginName,"
                      "AuthenticationPluginName,"
                      "UnlockSemantic,"
                      "CustomLockTimeoutMs,"
                      "AccessControlMode"
                    ")"
                    " VALUES ("
                      "?,?,?,?,?,?,?,?,?,?"
                    ");");

        Database::Query iq = m_db->prepare(insertSecretQuery, &errorText);
        if (!errorText.isEmpty()) {
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to prepare insert secret query: %1").arg(errorText));
        }

        QVariantList ivalues;
        ivalues << QVariant::fromValue<QString>(collectionName);
        ivalues << QVariant::fromValue<QString>(hashedSecretName);
        ivalues << QVariant::fromValue<QString>(collectionApplicationId);
        ivalues << QVariant::fromValue<int>(collectionUsesDeviceLockKey ? 1 : 0);
        ivalues << QVariant::fromValue<QString>(collectionStoragePluginName);
        ivalues << QVariant::fromValue<QString>(collectionEncryptionPluginName);
        ivalues << QVariant::fromValue<QString>(collectionAuthenticationPluginName);
        ivalues << QVariant::fromValue<int>(collectionUnlockSemantic);
        ivalues << QVariant::fromValue<int>(collectionCustomLockTimeoutMs);
        ivalues << QVariant::fromValue<int>(static_cast<int>(collectionAccessControlMode));
        iq.bindValues(ivalues);

        if (!m_db->beginTransaction()) {
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QLatin1String("Unable to begin insert secret transaction"));
        }

        if (!m_db->execute(iq, &errorText)) {
            m_db->rollbackTransaction();
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to execute insert secret query: %1").arg(errorText));
        }

        if (!m_db->commitTransaction()) {
            m_db->rollbackTransaction();
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QLatin1String("Unable to commit insert secret transaction"));
        }
    }

    Sailfish::Secrets::Result pluginResult;
    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
            if (locked) {
                pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(collectionName, authenticationKey);
                if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
                    // unable to apply the new authenticationKey.
                    m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(collectionName, QByteArray());
                    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::SecretsPluginDecryptionError,
                                                     QString::fromLatin1("Unable to decrypt collection %1 with the entered authentication key").arg(collectionName));

                }
                pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(collectionName, &locked);
                if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
                    m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(collectionName, QByteArray());
                    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::SecretsPluginDecryptionError,
                                                     QString::fromLatin1("Unable to check lock state of collection %1 after setting the entered authentication key").arg(collectionName));

                }
            }
            if (locked) {
                // still locked, even after applying the new authenticationKey?  The authenticationKey was wrong.
                m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(collectionName, QByteArray());
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::IncorrectAuthenticationKeyError,
                                                 QString::fromLatin1("The authentication key entered for collection %1 was incorrect").arg(collectionName));
            } else {
                // successfully unlocked the encrypted storage collection.  write the secret.
                pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->setSecret(collectionName, hashedSecretName, secret);
            }
        }
    } else {
        if (!m_collectionAuthenticationKeys.contains(collectionName)) {
            // TODO: some way to "test" the authenticationKey!
            m_collectionAuthenticationKeys.insert(collectionName, authenticationKey);
        }

        QByteArray encrypted;
        pluginResult = m_encryptionPlugins[collectionEncryptionPluginName]->encryptSecret(secret, m_collectionAuthenticationKeys.value(collectionName), &encrypted);
        if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
            pluginResult = m_storagePlugins[collectionStoragePluginName]->setSecret(collectionName, hashedSecretName, encrypted);
        }
    }

    if (pluginResult.code() == Sailfish::Secrets::Result::Failed && !secretAlreadyExists) {
        // The plugin was unable to set the secret in its storage.
        // Let's delete it from our master table if it was a new one.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->setSecret() call succeeds,
        // but the master table commit fails.
        const QString deleteSecretQuery = QStringLiteral(
                    "DELETE FROM Secrets"
                    " WHERE CollectionName = ?"
                    " AND SecretName = ?;");

        Database::Query dq = m_db->prepare(deleteSecretQuery, &errorText);
        if (!errorText.isEmpty()) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to prepare delete secret query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        QVariantList values;
        values << QVariant::fromValue<QString>(collectionName);
        values << QVariant::fromValue<QString>(hashedSecretName);
        dq.bindValues(values);

        if (!m_db->beginTransaction()) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to begin delete secret transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->execute(dq, &errorText)) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to execute delete secret query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->commitTransaction()) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to commit delete secret transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }
    }

    return pluginResult;
}

// set a standalone DeviceLock-protected secret
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::setStandaloneDeviceLockSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &secretName,
        const QByteArray &secret,
        Sailfish::Secrets::SecretManager::DeviceLockUnlockSemantic unlockSemantic,
        Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode)
{
    // TODO: Access Control requests to see if the application is permitted to set the secret.
    Q_UNUSED(requestId); // until we implement access control queries, this method is synchronous, so requestId is unused.
    Q_UNUSED(userInteractionMode);

    if (secretName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidSecretError,
                                         QLatin1String("Empty secret name given"));
    } else if (storagePluginName == encryptionPluginName && !m_encryptedStoragePlugins.contains(storagePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_storagePlugins.contains(storagePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_encryptionPlugins.contains(encryptionPluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(encryptionPluginName));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    const QString selectSecretsQuery = QStringLiteral(
                "SELECT"
                   " ApplicationId,"
                   " UsesDeviceLockKey,"
                   " StoragePluginName,"
                   " AccessControlMode"
                 " FROM Secrets"
                 " WHERE CollectionName = ?"
                 " AND SecretName = ?;"
             );

    QString errorText;
    Database::Query ssq = m_db->prepare(selectSecretsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select secrets query: %1").arg(errorText));
    }

    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);
    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    values << QVariant::fromValue<QString>(hashedSecretName);
    ssq.bindValues(values);

    if (!m_db->execute(ssq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select secrets query: %1").arg(errorText));
    }

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    Sailfish::Secrets::SecretManager::AccessControlMode secretAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (ssq.next()) {
        found = true;
        secretApplicationId = ssq.value(0).value<QString>();
        secretUsesDeviceLockKey = ssq.value(1).value<int>() > 0;
        secretStoragePluginName = ssq.value(2).value<QString>();
        secretAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(ssq.value(3).value<int>());
    }

    if (found && secretAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (found && secretApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Secret %1 is owned by a different application").arg(secretName));
    } else if (found && secretUsesDeviceLockKey == 0) {
        // don't update the secret if it would involve changing from a custom-lock to device-lock protected secret.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QString::fromLatin1("Secret %1 already exists and is not a devicelock protected secret")
                                         .arg(secretName));
    } else if (found && secretStoragePluginName.compare(storagePluginName, Qt::CaseInsensitive) != 0) {
        // don't update the secret if it would involve changing which plugin it's stored in.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QString::fromLatin1("Secret %1 already exists and is not stored via plugin %2")
                                         .arg(secretName, storagePluginName));
    }

    // Write to the master database prior to the storage plugin.

    const QString updateSecretQuery = QStringLiteral(
                 "UPDATE Secrets"
                 " SET ApplicationId = ?,"
                     " UsesDeviceLockKey = ?,"
                     " StoragePluginName = ?,"
                     " EncryptionPluginName = ?,"
                     " AuthenticationPluginName = ?,"
                     " UnlockSemantic = ?,"
                     " CustomLockTimeoutMs = ?,"
                     " AccessControlMode = ?"
                 " WHERE CollectionName = ?"
                 " AND SecretName = ?;"
             );

    const QString insertSecretQuery = QStringLiteral(
                "INSERT INTO Secrets ("
                  "CollectionName,"
                  "SecretName,"
                  "ApplicationId,"
                  "UsesDeviceLockKey,"
                  "StoragePluginName,"
                  "EncryptionPluginName,"
                  "AuthenticationPluginName,"
                  "UnlockSemantic,"
                  "CustomLockTimeoutMs,"
                  "AccessControlMode"
                ")"
                " VALUES ("
                  "?,?,?,?,?,?,?,?,?,?"
                ");");

    Database::Query iq = m_db->prepare(found ? updateSecretQuery : insertSecretQuery, &errorText);
    if (!errorText.isEmpty()) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare insert secret query: %1").arg(errorText));
    }

    QVariantList ivalues;
    if (!found) {
        ivalues << QVariant::fromValue<QString>(collectionName);
        ivalues << QVariant::fromValue<QString>(hashedSecretName);
    }
    ivalues << QVariant::fromValue<QString>(callerApplicationId);
    ivalues << QVariant::fromValue<int>(1);
    ivalues << QVariant::fromValue<QString>(storagePluginName);
    ivalues << QVariant::fromValue<QString>(encryptionPluginName);
    ivalues << QVariant::fromValue<QString>(Sailfish::Secrets::SecretManager::DefaultAuthenticationPluginName);
    ivalues << QVariant::fromValue<int>(static_cast<int>(unlockSemantic));
    ivalues << QVariant::fromValue<int>(0);
    ivalues << QVariant::fromValue<int>(static_cast<int>(accessControlMode));
    if (found) {
        ivalues << QVariant::fromValue<QString>(collectionName);
        ivalues << QVariant::fromValue<QString>(hashedSecretName);
    }
    iq.bindValues(ivalues);

    if (!m_db->beginTransaction()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to begin insert secret transaction"));
    }

    if (!m_db->execute(iq, &errorText)) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute insert secret query: %1").arg(errorText));
    }

    if (!m_db->commitTransaction()) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to commit insert secret transaction"));
    }

    Sailfish::Secrets::Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        // TODO: does the following work?  We'd need to add methods to the encrypted storage plugin: re-encryptStandaloneSecrets or something...
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->setSecret(collectionName, hashedSecretName, secret, DeviceLockKey);
    } else {
        QByteArray encrypted;
        pluginResult = m_encryptionPlugins[encryptionPluginName]->encryptSecret(secret, DeviceLockKey, &encrypted);
        if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
            pluginResult = m_storagePlugins[storagePluginName]->setSecret(collectionName, hashedSecretName, encrypted);
            if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
                m_standaloneSecretAuthenticationKeys.insert(hashedSecretName, DeviceLockKey);
            }
        }
    }

    if (pluginResult.code() == Sailfish::Secrets::Result::Failed && !found) {
        // The plugin was unable to set the secret in its storage.
        // Let's delete it from our master table if it was a new one.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->setSecret() call succeeds,
        // but the master table commit fails.
        const QString deleteSecretQuery = QStringLiteral(
                    "DELETE FROM Secrets"
                    " WHERE CollectionName = ?"
                    " AND SecretName = ?;");

        Database::Query dq = m_db->prepare(deleteSecretQuery, &errorText);
        if (!errorText.isEmpty()) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to prepare delete secret query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        QVariantList values;
        values << QVariant::fromValue<QString>(collectionName);
        values << QVariant::fromValue<QString>(hashedSecretName);
        dq.bindValues(values);

        if (!m_db->beginTransaction()) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to begin delete secret transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->execute(dq, &errorText)) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to execute delete secret query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->commitTransaction()) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to commit delete secret transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }
    }

    return pluginResult;
}

// set a standalone CustomLock-protected secret
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::setStandaloneCustomLockSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &authenticationPluginName,
        const QString &secretName,
        const QByteArray &secret,
        Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic unlockSemantic,
        int customLockTimeoutMs,
        Sailfish::Secrets::SecretManager::AccessControlMode accessControlMode,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress)
{
    if (secretName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidSecretError,
                                         QLatin1String("Empty secret name given"));
    } else if (storagePluginName == encryptionPluginName && !m_encryptedStoragePlugins.contains(storagePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_storagePlugins.contains(storagePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_encryptionPlugins.contains(encryptionPluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(encryptionPluginName));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    const QString selectSecretsQuery = QStringLiteral(
                "SELECT"
                   " ApplicationId,"
                   " UsesDeviceLockKey,"
                   " StoragePluginName,"
                   " AccessControlMode"
                 " FROM Secrets"
                 " WHERE CollectionName = ?"
                 " AND SecretName = ?;"
             );

    QString errorText;
    Database::Query ssq = m_db->prepare(selectSecretsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select secrets query: %1").arg(errorText));
    }

    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);
    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    values << QVariant::fromValue<QString>(hashedSecretName);
    ssq.bindValues(values);

    if (!m_db->execute(ssq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select secrets query: %1").arg(errorText));
    }

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    Sailfish::Secrets::SecretManager::AccessControlMode secretAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (ssq.next()) {
        found = true;
        secretApplicationId = ssq.value(0).value<QString>();
        secretUsesDeviceLockKey = ssq.value(1).value<int>() > 0;
        secretStoragePluginName = ssq.value(2).value<QString>();
        secretAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(ssq.value(3).value<int>());
    }

    if (found && secretAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (found && secretApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Secret %1 is owned by a different application").arg(secretName));
    } else if (found && secretUsesDeviceLockKey == 1) {
        // don't update the secret if it would involve changing from a device-lock to custom-lock protected secret.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QString::fromLatin1("Secret %1 already exists and is not a devicelock protected secret")
                                         .arg(secretName));
    } else if (found && secretStoragePluginName.compare(storagePluginName, Qt::CaseInsensitive) != 0) {
        // don't update the secret if it would involve changing which plugin it's stored in.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QString::fromLatin1("Secret %1 already exists and is not stored via plugin %2")
                                         .arg(secretName, storagePluginName));
    } else if (m_authenticationPlugins[authenticationPluginName]->authenticationType() == Sailfish::Secrets::AuthenticationPlugin::ApplicationSpecificAuthentication
               && (userInteractionMode != Sailfish::Secrets::SecretManager::InProcessUserInteractionMode || uiServiceAddress.isEmpty())) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresInProcessUserInteraction,
                                         QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(authenticationPluginName));
    } else if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                         QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(authenticationPluginName));
    }

    // perform the authentication required to get the authentication key which will be used
    // to encrypt the secret.
    Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[authenticationPluginName]->beginAuthentication(
                callerPid,
                requestId,
                callerApplicationId,
                QString(),
                secretName,
                uiServiceAddress);
    if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
        return authenticationResult;
    }

    m_pendingRequests.insert(requestId,
                             Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Sailfish::Secrets::Daemon::ApiImpl::SetStandaloneCustomLockSecretRequest,
                                 QVariantList() << storagePluginName
                                                << encryptionPluginName
                                                << authenticationPluginName
                                                << secretName
                                                << secret
                                                << unlockSemantic
                                                << customLockTimeoutMs
                                                << accessControlMode
                                                << userInteractionMode
                                                << uiServiceAddress));
    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
}

Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::setStandaloneCustomLockSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
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
        const QByteArray &authenticationKey)
{
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(uiServiceAddress);

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // check again in case some other application added the secret while
    // the asynchronous authentication plugin request was active.
    const QString selectSecretsQuery = QStringLiteral(
                "SELECT"
                   " ApplicationId,"
                   " UsesDeviceLockKey,"
                   " StoragePluginName,"
                   " AccessControlMode"
                 " FROM Secrets"
                 " WHERE CollectionName = ?"
                 " AND SecretName = ?;"
             );

    QString errorText;
    Database::Query ssq = m_db->prepare(selectSecretsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select secrets query: %1").arg(errorText));
    }

    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);
    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    values << QVariant::fromValue<QString>(hashedSecretName);
    ssq.bindValues(values);

    if (!m_db->execute(ssq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select secrets query: %1").arg(errorText));
    }

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    Sailfish::Secrets::SecretManager::AccessControlMode secretAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (ssq.next()) {
        found = true;
        secretApplicationId = ssq.value(0).value<QString>();
        secretUsesDeviceLockKey = ssq.value(1).value<int>() > 0;
        secretStoragePluginName = ssq.value(2).value<QString>();
        secretAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(ssq.value(3).value<int>());
    }

    if (found && secretAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (found && secretApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Secret %1 is owned by a different application").arg(secretName));
    } else if (found && secretUsesDeviceLockKey == 1) {
        // don't update the secret if it would involve changing from a device-lock to custom-lock protected secret.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QString::fromLatin1("Secret %1 already exists and is not a devicelock protected secret")
                                         .arg(secretName));
    } else if (found && secretStoragePluginName.compare(storagePluginName, Qt::CaseInsensitive) != 0) {
        // don't update the secret if it would involve changing which plugin it's stored in.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QString::fromLatin1("Secret %1 already exists and is not stored via plugin %2")
                                         .arg(secretName, storagePluginName));
    }

    // Write to the master database prior to the storage plugin.
    const QString updateSecretQuery = QStringLiteral(
                 "UPDATE Secrets"
                 " SET ApplicationId = ?,"
                     " UsesDeviceLockKey = ?,"
                     " StoragePluginName = ?,"
                     " EncryptionPluginName = ?,"
                     " AuthenticationPluginName = ?,"
                     " UnlockSemantic = ?,"
                     " CustomLockTimeoutMs = ?,"
                     " AccessControlMode = ?"
                 " WHERE CollectionName = ?"
                 " AND SecretName = ?;"
             );

    const QString insertSecretQuery = QStringLiteral(
                "INSERT INTO Secrets ("
                  "CollectionName,"
                  "SecretName,"
                  "ApplicationId,"
                  "UsesDeviceLockKey,"
                  "StoragePluginName,"
                  "EncryptionPluginName,"
                  "AuthenticationPluginName,"
                  "UnlockSemantic,"
                  "CustomLockTimeoutMs,"
                  "AccessControlMode"
                ")"
                " VALUES ("
                  "?,?,?,?,?,?,?,?,?,?"
                ");");

    Database::Query iq = m_db->prepare(found ? updateSecretQuery : insertSecretQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare insert secret query: %1").arg(errorText));
    }

    QVariantList ivalues;
    if (!found) {
        ivalues << QVariant::fromValue<QString>(collectionName);
        ivalues << QVariant::fromValue<QString>(hashedSecretName);
    }
    ivalues << QVariant::fromValue<QString>(callerApplicationId);
    ivalues << QVariant::fromValue<int>(1);
    ivalues << QVariant::fromValue<QString>(storagePluginName);
    ivalues << QVariant::fromValue<QString>(encryptionPluginName);
    ivalues << QVariant::fromValue<QString>(authenticationPluginName);
    ivalues << QVariant::fromValue<int>(static_cast<int>(unlockSemantic));
    ivalues << QVariant::fromValue<int>(customLockTimeoutMs);
    ivalues << QVariant::fromValue<int>(static_cast<int>(accessControlMode));
    if (found) {
        ivalues << QVariant::fromValue<QString>(collectionName);
        ivalues << QVariant::fromValue<QString>(hashedSecretName);
    }
    iq.bindValues(ivalues);

    if (!m_db->beginTransaction()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to begin insert secret transaction"));
    }

    if (!m_db->execute(iq, &errorText)) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute insert secret query: %1").arg(errorText));
    }

    if (!m_db->commitTransaction()) {
        m_db->rollbackTransaction();
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                         QLatin1String("Unable to commit insert secret transaction"));
    }

    Sailfish::Secrets::Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        // TODO: does the following work?  We'd need to add methods to the encrypted storage plugin: re-encryptStandaloneSecrets or something...
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->setSecret(collectionName, hashedSecretName, secret, authenticationKey);
    } else {
        QByteArray encrypted;
        pluginResult = m_encryptionPlugins[encryptionPluginName]->encryptSecret(secret, authenticationKey, &encrypted);
        if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
            pluginResult = m_storagePlugins[storagePluginName]->setSecret(collectionName, hashedSecretName, encrypted);
            if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
                m_standaloneSecretAuthenticationKeys.insert(hashedSecretName, authenticationKey);
            }
        }
    }

    if (pluginResult.code() == Sailfish::Secrets::Result::Failed && !found) {
        // The plugin was unable to set the secret in its storage.
        // Let's delete it from our master table if it was a new one.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->setSecret() call succeeds,
        // but the master table commit fails.
        const QString deleteSecretQuery = QStringLiteral(
                    "DELETE FROM Secrets"
                    " WHERE CollectionName = ?"
                    " AND SecretName = ?;");

        Database::Query dq = m_db->prepare(deleteSecretQuery, &errorText);
        if (!errorText.isEmpty()) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to prepare delete secret query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        QVariantList values;
        values << QVariant::fromValue<QString>(collectionName);
        values << QVariant::fromValue<QString>(hashedSecretName);
        dq.bindValues(values);

        if (!m_db->beginTransaction()) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to begin delete secret transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->execute(dq, &errorText)) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to execute delete secret query: %1"
                                                                 " while removing artifacts due to plugin operation failure: %2: %3")
                                             .arg(errorText).arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }

        if (!m_db->commitTransaction()) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to commit delete secret transaction"
                                                                 " while removing artifacts due to plugin operation failure: %1: %2")
                                             .arg(pluginResult.errorCode()).arg(pluginResult.errorMessage()));
        }
    }

    return pluginResult;
}

// get a secret in a collection
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::getCollectionSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &secretName,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress,
        QByteArray *secret)
{
    if (secretName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidSecretError,
                                         QLatin1String("Empty secret name given"));
    } else if (collectionName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Empty collection name given"));
    } else if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Reserved collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    const QString selectCollectionsQuery = QStringLiteral(
                 "SELECT"
                    " ApplicationId,"
                    " UsesDeviceLockKey,"
                    " StoragePluginName,"
                    " EncryptionPluginName,"
                    " AuthenticationPluginName,"
                    " UnlockSemantic,"
                    " CustomLockTimeoutMs,"
                    " AccessControlMode"
                  " FROM Collections"
                  " WHERE CollectionName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectCollectionsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select collections query: %1").arg(errorText));
    }

    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select collections query: %1").arg(errorText));
    }

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    int collectionUnlockSemantic = 0;
    int collectionCustomLockTimeoutMs = 0;
    Sailfish::Secrets::SecretManager::AccessControlMode collectionAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (sq.next()) {
        found = true;
        collectionApplicationId = sq.value(0).value<QString>();
        collectionUsesDeviceLockKey = sq.value(1).value<int>() > 0;
        collectionStoragePluginName = sq.value(2).value<QString>();
        collectionEncryptionPluginName = sq.value(3).value<QString>();
        collectionAuthenticationPluginName = sq.value(4).value<QString>();
        collectionUnlockSemantic = sq.value(5).value<int>();
        collectionCustomLockTimeoutMs = sq.value(6).value<int>();
        collectionAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(sq.value(7).value<int>());
    }

    if (!found) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Nonexistent collection name given"));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName && !m_storagePlugins.contains(collectionStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName && !m_encryptionPlugins.contains(collectionEncryptionPluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(collectionEncryptionPluginName));
    } else if (collectionAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Collection %1 is owned by a different application").arg(collectionName));
    } else if (!m_authenticationPlugins.contains(collectionAuthenticationPluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                        QString::fromLatin1("No such authentication plugin available: %1").arg(collectionAuthenticationPluginName));
    } else if (m_authenticationPlugins[collectionAuthenticationPluginName]->authenticationType() == Sailfish::Secrets::AuthenticationPlugin::ApplicationSpecificAuthentication
                   && (userInteractionMode != Sailfish::Secrets::SecretManager::InProcessUserInteractionMode || uiServiceAddress.isEmpty())) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresInProcessUserInteraction,
                                     QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(collectionAuthenticationPluginName));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        Sailfish::Secrets::Result pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
            return pluginResult;
        }

        if (locked) {
            if (collectionUsesDeviceLockKey) {
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionIsLockedError,
                                                 QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(collectionName));
            } else {
                if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
                    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                                     QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
                }

                // perform UI request to get the authentication key for the collection
                Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginAuthentication(
                            callerPid,
                            requestId,
                            callerApplicationId,
                            collectionName,
                            secretName,
                            uiServiceAddress);
                if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
                    return authenticationResult;
                }

                m_pendingRequests.insert(requestId,
                                         Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                             callerPid,
                                             requestId,
                                             Sailfish::Secrets::Daemon::ApiImpl::GetCollectionSecretRequest,
                                             QVariantList() << collectionName
                                                            << secretName
                                                            << userInteractionMode
                                                            << uiServiceAddress
                                                            << collectionStoragePluginName
                                                            << collectionEncryptionPluginName
                                                            << collectionUnlockSemantic
                                                            << collectionCustomLockTimeoutMs));
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
            }
        } else {
            return getCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        collectionName,
                        secretName,
                        userInteractionMode,
                        uiServiceAddress,
                        collectionStoragePluginName,
                        collectionEncryptionPluginName,
                        collectionUnlockSemantic,
                        collectionCustomLockTimeoutMs,
                        QByteArray(), // no key required, it's unlocked already.
                        secret);
        }
    } else {
        if (!m_collectionAuthenticationKeys.contains(collectionName)) {
            if (collectionUsesDeviceLockKey) {
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionIsLockedError,
                                                 QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(collectionName));
            } else {
                if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
                    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                                     QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
                }

                // perform UI request to get the authentication key for the collection
                Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginAuthentication(
                            callerPid,
                            requestId,
                            callerApplicationId,
                            collectionName,
                            secretName,
                            uiServiceAddress);
                if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
                    return authenticationResult;
                }

                m_pendingRequests.insert(requestId,
                                         Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                             callerPid,
                                             requestId,
                                             Sailfish::Secrets::Daemon::ApiImpl::GetCollectionSecretRequest,
                                             QVariantList() << collectionName
                                                            << secretName
                                                            << userInteractionMode
                                                            << uiServiceAddress
                                                            << collectionStoragePluginName
                                                            << collectionEncryptionPluginName
                                                            << collectionUnlockSemantic
                                                            << collectionCustomLockTimeoutMs));
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
            }
        } else {
            return getCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        collectionName,
                        secretName,
                        userInteractionMode,
                        uiServiceAddress,
                        collectionStoragePluginName,
                        collectionEncryptionPluginName,
                        collectionUnlockSemantic,
                        collectionCustomLockTimeoutMs,
                        m_collectionAuthenticationKeys.value(collectionName),
                        secret);
        }
    }
}

Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::getCollectionSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &secretName,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        int collectionUnlockSemantic,
        int collectionCustomLockTimeoutMs,
        const QByteArray &authenticationKey,
        QByteArray *secret)
{
    // might be required in future for access control requests.
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(uiServiceAddress);

    if (collectionUnlockSemantic == Sailfish::Secrets::SecretManager::CustomLockTimoutRelock) {
        if (!m_collectionLockTimers.contains(collectionName)) {
            QTimer *timer = new QTimer(this);
            connect(timer, &QTimer::timeout,
                    this, &Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::timeoutRelockCollection);
            timer->setInterval(collectionCustomLockTimeoutMs);
            timer->setSingleShot(true);
            timer->start();
            m_collectionLockTimers.insert(collectionName, timer);
        }
    }

    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);
    Sailfish::Secrets::Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
            return pluginResult;
        }
        // if it's locked, attempt to unlock it
        if (locked) {
            pluginResult = m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(collectionName, authenticationKey);
            if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
                // unable to apply the new authenticationKey.
                m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(collectionName, QByteArray());
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::SecretsPluginDecryptionError,
                                                 QString::fromLatin1("Unable to decrypt collection %1 with the entered authentication key").arg(collectionName));

            }
            pluginResult = m_encryptedStoragePlugins[storagePluginName]->isLocked(collectionName, &locked);
            if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
                m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(collectionName, QByteArray());
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::SecretsPluginDecryptionError,
                                                 QString::fromLatin1("Unable to check lock state of collection %1 after setting the entered authentication key").arg(collectionName));

            }
        }
        if (locked) {
            // still locked, even after applying the new authenticationKey?  The authenticationKey was wrong.
            m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(collectionName, QByteArray());
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::IncorrectAuthenticationKeyError,
                                             QString::fromLatin1("The authentication key entered for collection %1 was incorrect").arg(collectionName));
        }
        // successfully unlocked the encrypted storage collection.  write the secret.
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->getSecret(collectionName, hashedSecretName, secret);
    } else {
        if (!m_collectionAuthenticationKeys.contains(collectionName)) {
            // TODO: some way to "test" the authenticationKey!  also, if it's a custom lock, set the timeout, etc.
            m_collectionAuthenticationKeys.insert(collectionName, authenticationKey);
        }

        QByteArray encrypted;
        pluginResult = m_storagePlugins[storagePluginName]->getSecret(collectionName, hashedSecretName, &encrypted);
        if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
            pluginResult = m_encryptionPlugins[encryptionPluginName]->decryptSecret(encrypted, m_collectionAuthenticationKeys.value(collectionName), secret);
        }
    }

    return pluginResult;
}

// get a standalone secret
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::getStandaloneSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &secretName,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress,
        QByteArray *secret)
{
    if (secretName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidSecretError,
                                         QLatin1String("Empty secret name given"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    const QString selectSecretsQuery = QStringLiteral(
                 "SELECT"
                    " ApplicationId,"
                    " UsesDeviceLockKey,"
                    " StoragePluginName,"
                    " EncryptionPluginName,"
                    " AuthenticationPluginName,"
                    " UnlockSemantic,"
                    " CustomLockTimeoutMs,"
                    " AccessControlMode"
                  " FROM Secrets"
                  " WHERE CollectionName = ?"
                  " AND SecretName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectSecretsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select secrets query: %1").arg(errorText));
    }

    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);
    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    values << QVariant::fromValue<QString>(hashedSecretName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select secrets query: %1").arg(errorText));
    }

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    QString secretEncryptionPluginName;
    QString secretAuthenticationPluginName;
    int secretUnlockSemantic = 0;
    int secretCustomLockTimeoutMs = 0;
    Sailfish::Secrets::SecretManager::AccessControlMode secretAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (sq.next()) {
        found = true;
        secretApplicationId = sq.value(0).value<QString>();
        secretUsesDeviceLockKey = sq.value(1).value<int>() > 0;
        secretStoragePluginName = sq.value(2).value<QString>();
        secretEncryptionPluginName = sq.value(3).value<QString>();
        secretAuthenticationPluginName = sq.value(4).value<QString>();
        secretUnlockSemantic = sq.value(5).value<int>();
        secretCustomLockTimeoutMs = sq.value(6).value<int>();
        secretAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(sq.value(7).value<int>());
    }

    if (!found) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidSecretError,
                                         QLatin1String("Nonexistent secret name given"));
    }

    if (secretStoragePluginName == secretEncryptionPluginName && !m_encryptedStoragePlugins.contains(secretStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(secretStoragePluginName));
    } else if (secretStoragePluginName != secretEncryptionPluginName && !m_storagePlugins.contains(secretStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(secretStoragePluginName));
    } else if (secretStoragePluginName != secretEncryptionPluginName && !m_encryptionPlugins.contains(secretEncryptionPluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(secretEncryptionPluginName));
    } else if (secretAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (secretApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Secret %1 is owned by a different application").arg(secretName));
    } else if (m_authenticationPlugins[secretAuthenticationPluginName]->authenticationType() == Sailfish::Secrets::AuthenticationPlugin::ApplicationSpecificAuthentication
               && (userInteractionMode != Sailfish::Secrets::SecretManager::InProcessUserInteractionMode || uiServiceAddress.isEmpty())) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresInProcessUserInteraction,
                                         QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(secretAuthenticationPluginName));
    }

    if (m_standaloneSecretAuthenticationKeys.contains(hashedSecretName)) {
        return getStandaloneSecretWithAuthenticationKey(
                    callerPid,
                    requestId,
                    secretName,
                    userInteractionMode,
                    uiServiceAddress,
                    secretStoragePluginName,
                    secretEncryptionPluginName,
                    secretUnlockSemantic,
                    secretCustomLockTimeoutMs,
                    m_standaloneSecretAuthenticationKeys.value(hashedSecretName),
                    secret);
    }

    if (secretUsesDeviceLockKey) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionIsLockedError,
                                         QString::fromLatin1("Secret %1 is locked and requires device lock authentication").arg(secretName));
    }

    if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                         QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(secretAuthenticationPluginName));
    }

    // perform UI request to get the authentication key for the collection
    Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[secretAuthenticationPluginName]->beginAuthentication(
                callerPid,
                requestId,
                callerApplicationId,
                QString(),
                secretName,
                uiServiceAddress);
    if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
        return authenticationResult;
    }

    m_pendingRequests.insert(requestId,
                             Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Sailfish::Secrets::Daemon::ApiImpl::GetStandaloneSecretRequest,
                                 QVariantList() << collectionName
                                                << secretName
                                                << userInteractionMode
                                                << uiServiceAddress
                                                << secretStoragePluginName
                                                << secretEncryptionPluginName
                                                << secretUnlockSemantic
                                                << secretCustomLockTimeoutMs));
    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
}

Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::getStandaloneSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &secretName,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        int secretUnlockSemantic,
        int secretCustomLockTimeoutMs,
        const QByteArray &authenticationKey,
        QByteArray *secret)
{
    // may be needed for access control requests in the future.
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(uiServiceAddress);

    if (secretUnlockSemantic == Sailfish::Secrets::SecretManager::CustomLockTimoutRelock) {
        if (!m_standaloneSecretLockTimers.contains(secretName)) {
            QTimer *timer = new QTimer(this);
            connect(timer, &QTimer::timeout,
                    this, &Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::timeoutRelockSecret);
            timer->setInterval(secretCustomLockTimeoutMs);
            timer->setSingleShot(true);
            timer->start();
            m_standaloneSecretLockTimers.insert(secretName, timer);
        }
    }

    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);

    Sailfish::Secrets::Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->accessSecret(collectionName, hashedSecretName, authenticationKey, secret);
    } else {
        QByteArray encrypted;
        pluginResult = m_storagePlugins[storagePluginName]->getSecret(collectionName, hashedSecretName, &encrypted);
        if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
            pluginResult = m_encryptionPlugins[encryptionPluginName]->decryptSecret(encrypted, authenticationKey, secret);
        }
    }

    return pluginResult;
}

// delete a secret in a collection
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::deleteCollectionSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &secretName,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress)
{
    if (secretName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidSecretError,
                                         QLatin1String("Empty secret name given"));
    } else if (collectionName.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Empty collection name given"));
    } else if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Reserved collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    const QString selectCollectionsQuery = QStringLiteral(
                 "SELECT"
                    " ApplicationId,"
                    " UsesDeviceLockKey,"
                    " StoragePluginName,"
                    " EncryptionPluginName,"
                    " AuthenticationPluginName,"
                    " AccessControlMode"
                  " FROM Collections"
                  " WHERE CollectionName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectCollectionsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select collections query: %1").arg(errorText));
    }

    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select collections query: %1").arg(errorText));
    }

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    Sailfish::Secrets::SecretManager::AccessControlMode collectionAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (sq.next()) {
        found = true;
        collectionApplicationId = sq.value(0).value<QString>();
        collectionUsesDeviceLockKey = sq.value(1).value<int>() > 0;
        collectionStoragePluginName = sq.value(2).value<QString>();
        collectionEncryptionPluginName = sq.value(3).value<QString>();
        collectionAuthenticationPluginName = sq.value(4).value<QString>();
        collectionAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(sq.value(5).value<int>());
    }

    if (!found) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Nonexistent collection name given"));
    }

    if (collectionAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Collection %1 is owned by a different application").arg(collectionName));
    } else if (collectionStoragePluginName == collectionEncryptionPluginName
            && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionStoragePluginName.isEmpty() || !m_storagePlugins.contains(collectionStoragePluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionEncryptionPluginName.isEmpty() || !m_encryptionPlugins.contains(collectionEncryptionPluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(collectionEncryptionPluginName));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        Sailfish::Secrets::Result pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
            return pluginResult;
        }
        if (locked) {
            if (collectionUsesDeviceLockKey) {
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionIsLockedError,
                                                 QString::fromLatin1("Collection %1 is locked and requires device lock authentication"));
            }

            if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                                 QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
            }

            // perform UI request to get the authentication key for the collection
            Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginAuthentication(
                        callerPid,
                        requestId,
                        callerApplicationId,
                        collectionName,
                        secretName,
                        uiServiceAddress);
            if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
                return authenticationResult;
            }

            m_pendingRequests.insert(requestId,
                                     Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                         callerPid,
                                         requestId,
                                         Sailfish::Secrets::Daemon::ApiImpl::DeleteCollectionSecretRequest,
                                         QVariantList() << collectionName
                                                        << secretName
                                                        << userInteractionMode
                                                        << uiServiceAddress));
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
        } else {
            return deleteCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        collectionName,
                        secretName,
                        userInteractionMode,
                        uiServiceAddress,
                        QByteArray());
        }
    } else {
        if (!m_collectionAuthenticationKeys.contains(collectionName)) {
            if (collectionUsesDeviceLockKey) {
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::CollectionIsLockedError,
                                                 QStringLiteral("Collection %1 is locked and requires device lock authentication"));
            } else {
                if (userInteractionMode == Sailfish::Secrets::SecretManager::PreventUserInteractionMode) {
                    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationRequiresUserInteraction,
                                                     QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
                }

                // perform UI request to get the authentication key for the collection
                Sailfish::Secrets::Result authenticationResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginAuthentication(
                            callerPid,
                            requestId,
                            callerApplicationId,
                            collectionName,
                            secretName,
                            uiServiceAddress);
                if (authenticationResult.code() == Sailfish::Secrets::Result::Failed) {
                    return authenticationResult;
                }

                m_pendingRequests.insert(requestId,
                                         Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                             callerPid,
                                             requestId,
                                             Sailfish::Secrets::Daemon::ApiImpl::DeleteCollectionSecretRequest,
                                             QVariantList() << collectionName
                                                            << secretName
                                                            << userInteractionMode
                                                            << uiServiceAddress));
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Pending);
            }
        } else {
            return deleteCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        collectionName,
                        secretName,
                        userInteractionMode,
                        uiServiceAddress,
                        m_collectionAuthenticationKeys.value(collectionName));
        }
    }
}

Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::deleteCollectionSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &secretName,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode,
        const QString &uiServiceAddress,
        const QByteArray &authenticationKey)
{
    // may be needed for access control requests in the future.
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(uiServiceAddress);

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // check again in case it was deleted or modified while the
    // asynchronous authentication key request was in progress.
    const QString selectCollectionsQuery = QStringLiteral(
                 "SELECT"
                    " ApplicationId,"
                    " UsesDeviceLockKey,"
                    " StoragePluginName,"
                    " EncryptionPluginName,"
                    " AuthenticationPluginName,"
                    " AccessControlMode"
                  " FROM Collections"
                  " WHERE CollectionName = ?;"
             );

    QString errorText;
    Database::Query sq = m_db->prepare(selectCollectionsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select collections query: %1").arg(errorText));
    }

    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    sq.bindValues(values);

    if (!m_db->execute(sq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select collections query: %1").arg(errorText));
    }

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    Sailfish::Secrets::SecretManager::AccessControlMode collectionAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (sq.next()) {
        found = true;
        collectionApplicationId = sq.value(0).value<QString>();
        collectionUsesDeviceLockKey = sq.value(1).value<int>() > 0;
        collectionStoragePluginName = sq.value(2).value<QString>();
        collectionEncryptionPluginName = sq.value(3).value<QString>();
        collectionAuthenticationPluginName = sq.value(4).value<QString>();
        collectionAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(sq.value(5).value<int>());
    }

    if (!found) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidCollectionError,
                                         QLatin1String("Nonexistent collection name given"));
    }

    if (collectionUsesDeviceLockKey && authenticationKey != DeviceLockKey) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::IncorrectAuthenticationKeyError,
                                         QLatin1String("Incorrect device lock key provided"));
    }

    if (collectionAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Collection %1 is owned by a different application").arg(collectionName));
    }

    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);
    Sailfish::Secrets::Result pluginResult;
    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
            return pluginResult;
        }
        // if it's locked, attempt to unlock it
        if (locked) {
            pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(collectionName, authenticationKey);
            if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
                // unable to apply the new authenticationKey.
                m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(collectionName, QByteArray());
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::SecretsPluginDecryptionError,
                                                 QString::fromLatin1("Unable to decrypt collection %1 with the entered authentication key").arg(collectionName));

            }
            pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(collectionName, &locked);
            if (pluginResult.code() != Sailfish::Secrets::Result::Succeeded) {
                m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(collectionName, QByteArray());
                return Sailfish::Secrets::Result(Sailfish::Secrets::Result::SecretsPluginDecryptionError,
                                                 QString::fromLatin1("Unable to check lock state of collection %1 after setting the entered authentication key").arg(collectionName));

            }
        }
        if (locked) {
            // still locked, even after applying the new authenticationKey?  The authenticationKey was wrong.
            m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(collectionName, QByteArray());
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::IncorrectAuthenticationKeyError,
                                             QString::fromLatin1("The authentication key entered for collection %1 was incorrect").arg(collectionName));
        }
        // successfully unlocked the encrypted storage collection.  remove the secret.
        pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->removeSecret(collectionName, hashedSecretName);
    } else {
        if (!m_collectionAuthenticationKeys.contains(collectionName)) {
            // TODO: some way to "test" the authenticationKey!  also, if it's a custom lock, set the timeout, etc.
            m_collectionAuthenticationKeys.insert(collectionName, authenticationKey);
        }

        pluginResult = m_storagePlugins[collectionStoragePluginName]->removeSecret(collectionName, hashedSecretName);
    }

    // now remove from the master database.
    if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
        const QString deleteSecretQuery = QStringLiteral(
                    "DELETE FROM Secrets"
                    " WHERE CollectionName = ?"
                    " AND SecretName = ?;");

        Database::Query dq = m_db->prepare(deleteSecretQuery, &errorText);
        if (!errorText.isEmpty()) {
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to prepare delete secret query: %1")
                                             .arg(errorText));
        }

        QVariantList values;
        values << QVariant::fromValue<QString>(collectionName);
        values << QVariant::fromValue<QString>(hashedSecretName);
        dq.bindValues(values);

        if (!m_db->beginTransaction()) {
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QLatin1String("Unable to begin delete secret transaction"));
        }

        if (!m_db->execute(dq, &errorText)) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to execute delete secret query: %1")
                                             .arg(errorText));
        }

        if (!m_db->commitTransaction()) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QLatin1String("Unable to commit delete secret transaction"));
        }
    }

    if (collectionAccessControlMode == Sailfish::Secrets::SecretManager::SystemAccessControlMode) {
        // TODO: tell AccessControl daemon to remove this datum from its database.
    }

    return pluginResult;
}

// delete a standalone secret
Sailfish::Secrets::Result
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::deleteStandaloneSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &secretName,
        Sailfish::Secrets::SecretManager::UserInteractionMode userInteractionMode)
{
    // these may be required in the future for access control requests.
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // check again in case some other application added the secret while
    // the asynchronous authentication plugin request was active.
    const QString selectSecretsQuery = QStringLiteral(
                "SELECT"
                   " ApplicationId,"
                   " UsesDeviceLockKey,"
                   " StoragePluginName,"
                   " EncryptionPluginName,"
                   " AccessControlMode"
                 " FROM Secrets"
                 " WHERE CollectionName = ?"
                 " AND SecretName = ?;"
             );

    QString errorText;
    Database::Query ssq = m_db->prepare(selectSecretsQuery, &errorText);
    if (!errorText.isEmpty()) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to prepare select secrets query: %1").arg(errorText));
    }

    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = generateHashedSecretName(collectionName, secretName);
    QVariantList values;
    values << QVariant::fromValue<QString>(collectionName);
    values << QVariant::fromValue<QString>(hashedSecretName);
    ssq.bindValues(values);

    if (!m_db->execute(ssq, &errorText)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                         QString::fromLatin1("Unable to execute select secrets query: %1").arg(errorText));
    }

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    QString secretEncryptionPluginName;
    Sailfish::Secrets::SecretManager::AccessControlMode secretAccessControlMode = Sailfish::Secrets::SecretManager::OwnerOnlyMode;
    if (ssq.next()) {
        found = true;
        secretApplicationId = ssq.value(0).value<QString>();
        secretUsesDeviceLockKey = ssq.value(1).value<int>() > 0;
        secretStoragePluginName = ssq.value(2).value<QString>();
        secretEncryptionPluginName = ssq.value(3).value<QString>();
        secretAccessControlMode = static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(ssq.value(4).value<int>());
    }

    if (!found) {
        // the secret doesn't exist, return success.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Succeeded);
    }

    if (secretAccessControlMode != Sailfish::Secrets::SecretManager::OwnerOnlyMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::OperationNotSupportedError,
                                         QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (secretApplicationId != callerApplicationId) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::PermissionsError,
                                         QString::fromLatin1("Secret %1 is owned by a different application").arg(secretName));
    } else if (secretStoragePluginName == secretEncryptionPluginName && !m_encryptedStoragePlugins.contains(secretStoragePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(secretStoragePluginName));
    } else if (secretStoragePluginName != secretEncryptionPluginName && !m_storagePlugins.contains(secretStoragePluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such storage plugin exists: %1").arg(secretStoragePluginName));
    } else if (secretStoragePluginName != secretEncryptionPluginName && !m_encryptionPlugins.contains(secretEncryptionPluginName)) {
        return Sailfish::Secrets::Result(Sailfish::Secrets::Result::InvalidExtensionPluginError,
                                         QString::fromLatin1("No such encryption plugin exists: %1").arg(secretEncryptionPluginName));
    }

    Sailfish::Secrets::Result pluginResult;
    if (secretStoragePluginName == secretEncryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[secretStoragePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() == Sailfish::Secrets::Result::Failed) {
            return pluginResult;
        }
        if (locked && secretUsesDeviceLockKey) {
            pluginResult = m_encryptedStoragePlugins[secretStoragePluginName]->setEncryptionKey(collectionName, DeviceLockKey);
            if (pluginResult.code() == Sailfish::Secrets::Result::Failed) {
                return pluginResult;
            }
        }
        pluginResult = m_encryptedStoragePlugins[secretStoragePluginName]->removeSecret(collectionName, hashedSecretName);
        if (locked) {
            // relock after delete-access.
            m_encryptedStoragePlugins[secretStoragePluginName]->setEncryptionKey(collectionName, QByteArray());
        }
    } else {
        pluginResult = m_storagePlugins[secretStoragePluginName]->removeSecret(collectionName, hashedSecretName);
        if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
            m_standaloneSecretAuthenticationKeys.remove(hashedSecretName);
            m_standaloneSecretLockTimers.remove(hashedSecretName);
        }
    }

    // remove from master database also.
    if (pluginResult.code() == Sailfish::Secrets::Result::Succeeded) {
        const QString deleteSecretQuery = QStringLiteral(
                    "DELETE FROM Secrets"
                    " WHERE CollectionName = ?"
                    " AND SecretName = ?;");

        Database::Query dq = m_db->prepare(deleteSecretQuery, &errorText);
        if (!errorText.isEmpty()) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to prepare delete secret query: %1")
                                             .arg(errorText));
        }

        QVariantList values;
        values << QVariant::fromValue<QString>(collectionName);
        values << QVariant::fromValue<QString>(hashedSecretName);
        dq.bindValues(values);

        if (!m_db->beginTransaction()) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to begin delete secret transaction"));
        }

        if (!m_db->execute(dq, &errorText)) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseQueryError,
                                             QString::fromLatin1("Unable to execute delete secret query: %1")
                                             .arg(errorText));
        }

        if (!m_db->commitTransaction()) {
            m_db->rollbackTransaction();
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return Sailfish::Secrets::Result(Sailfish::Secrets::Result::DatabaseTransactionError,
                                             QString::fromLatin1("Unable to commit delete secret transaction"));
        }
    }

    return Sailfish::Secrets::Result(Sailfish::Secrets::Result::Succeeded);
}

void
Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::authenticationCompleted(
        uint callerPid,
        qint64 requestId,
        const QString &callerApplicationId,
        const QString &collectionName,
        const QString &secretName,
        const QString &uiServiceAddress,
        const Sailfish::Secrets::Result &result,
        const QByteArray &authenticationKey)
{
    // may be needed in the future for "multiple-step" flows.
    Q_UNUSED(callerPid);
    Q_UNUSED(callerApplicationId);
    Q_UNUSED(collectionName);
    Q_UNUSED(secretName);
    Q_UNUSED(uiServiceAddress);

    QByteArray secret;
    Sailfish::Secrets::Result returnResult = result;
    if (result.code() == Sailfish::Secrets::Result::Succeeded) {
        // look up the pending request in our list
        if (m_pendingRequests.contains(requestId)) {
            // call the appropriate method to complete the request
            Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::PendingRequest pr = m_pendingRequests.take(requestId);
            switch (pr.requestType) {
                case CreateCustomLockCollectionRequest: {
                    if (pr.parameters.size() != 9) {
                        returnResult = Sailfish::Secrets::Result(Sailfish::Secrets::Result::UnknownError,
                                                                 QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = createCustomLockCollectionWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    static_cast<Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<int>(),
                                    static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()),
                                    static_cast<Sailfish::Secrets::SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    authenticationKey);
                    }
                    break;
                }
                case SetCollectionSecretRequest: {
                    if (pr.parameters.size() != 13) {
                        returnResult = Sailfish::Secrets::Result(Sailfish::Secrets::Result::UnknownError,
                                                                 QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = setCollectionSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QByteArray>(),
                                    static_cast<Sailfish::Secrets::SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<bool>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()),
                                    authenticationKey);
                    }
                    break;
                }
                case SetStandaloneCustomLockSecretRequest: {
                    if (pr.parameters.size() != 10) {
                        returnResult = Sailfish::Secrets::Result(Sailfish::Secrets::Result::UnknownError,
                                                                 QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = setStandaloneCustomLockSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QByteArray>(),
                                    static_cast<Sailfish::Secrets::SecretManager::CustomLockUnlockSemantic>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<int>(),
                                    static_cast<Sailfish::Secrets::SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()),
                                    static_cast<Sailfish::Secrets::SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    authenticationKey);
                    }
                    break;
                }
                case GetCollectionSecretRequest: {
                    if (pr.parameters.size() != 8) {
                        returnResult = Sailfish::Secrets::Result(Sailfish::Secrets::Result::UnknownError,
                                                                 QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = getCollectionSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    static_cast<Sailfish::Secrets::SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    authenticationKey,
                                    &secret);
                    }
                    break;
                }
                case GetStandaloneSecretRequest: {
                    if (pr.parameters.size() != 7) {
                        returnResult = Sailfish::Secrets::Result(Sailfish::Secrets::Result::UnknownError,
                                                                 QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = getStandaloneSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    static_cast<Sailfish::Secrets::SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    authenticationKey,
                                    &secret);
                    }
                    break;
                }
                case DeleteCollectionSecretRequest: {
                    if (pr.parameters.size() != 4) {
                        returnResult = Sailfish::Secrets::Result(Sailfish::Secrets::Result::UnknownError,
                                                                 QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = deleteCollectionSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    static_cast<Sailfish::Secrets::SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    authenticationKey);
                    }
                    break;
                }
                default: {
                    returnResult = Sailfish::Secrets::Result(Sailfish::Secrets::Result::UnknownError,
                                                             QLatin1String("Internal error: unknown continuation for asynchronous request!"));
                    break;
                }
            }
        } else {
            returnResult = Sailfish::Secrets::Result(Sailfish::Secrets::Result::UnknownError,
                                                     QLatin1String("Internal error: failed to finish unknown pending request!"));
        }
    }

    // finish the request.
    QList<QVariant> outParams;
    outParams << QVariant::fromValue<Sailfish::Secrets::Result>(returnResult);
    outParams << QVariant::fromValue<QByteArray>(secret);
    m_requestQueue->requestFinished(requestId, outParams);
}

void Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::timeoutRelockCollection()
{
    QTimer *timer = qobject_cast<QTimer*>(sender());
    for (QMap<QString, QTimer*>::iterator it = m_collectionLockTimers.begin(); it != m_collectionLockTimers.end(); it++) {
        if (it.value() == timer) {
            qCDebug(lcSailfishSecretsDaemon) << "Relocking collection:" << it.key() << "due to unlock timeout!";
            m_collectionAuthenticationKeys.remove(it.key());
            m_collectionLockTimers.erase(it);
            break;
        }
    }
    timer->deleteLater();
}

void Sailfish::Secrets::Daemon::ApiImpl::RequestProcessor::timeoutRelockSecret()
{
    QTimer *timer = qobject_cast<QTimer*>(sender());
    for (QMap<QString, QTimer*>::iterator it = m_standaloneSecretLockTimers.begin(); it != m_standaloneSecretLockTimers.end(); it++) {
        if (it.value() == timer) {
            qCDebug(lcSailfishSecretsDaemon) << "Relocking standalone secret:" << it.key() << "due to unlock timeout!";
            m_standaloneSecretAuthenticationKeys.remove(it.key());
            m_standaloneSecretLockTimers.erase(it);
            break;
        }
    }
    timer->deleteLater();
}

