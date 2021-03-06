/*
 * Copyright (C) 2017 Jolla Ltd.
 * Contact: Chris Adams <chris.adams@jollamobile.com>
 * All rights reserved.
 * BSD 3-Clause License, see LICENSE.
 */

#ifndef SAILFISHSECRETS_PLUGIN_STORAGE_SQLITE_H
#define SAILFISHSECRETS_PLUGIN_STORAGE_SQLITE_H

#include "Secrets/extensionplugins.h"
#include "Secrets/result.h"

#include "database_p.h"

#include <QObject>
#include <QVector>
#include <QString>
#include <QByteArray>
#include <QCryptographicHash>
#include <QMutexLocker>

namespace Sailfish {

namespace Secrets {

namespace Daemon {

namespace Plugins {

class Q_DECL_EXPORT SqlitePlugin : public Sailfish::Secrets::StoragePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID Sailfish_Secrets_StoragePlugin_IID)
    Q_INTERFACES(Sailfish::Secrets::StoragePlugin)

public:
    SqlitePlugin(QObject *parent = Q_NULLPTR);
    ~SqlitePlugin();

    bool isTestPlugin() const Q_DECL_OVERRIDE {
#ifdef SAILFISH_SECRETS_BUILD_TEST_PLUGIN
        return true;
#else
        return false;
#endif
    }

    QString name() const Q_DECL_OVERRIDE { return QLatin1String("org.sailfishos.secrets.plugin.storage.sqlite"); }
    Sailfish::Secrets::StoragePlugin::StorageType storageType() const Q_DECL_OVERRIDE { return Sailfish::Secrets::StoragePlugin::FileSystemStorage; }

    Sailfish::Secrets::Result createCollection(const QString &collectionName) Q_DECL_OVERRIDE;
    Sailfish::Secrets::Result removeCollection(const QString &collectionName) Q_DECL_OVERRIDE;
    Sailfish::Secrets::Result setSecret(const QString &collectionName, const QString &secretName, const QByteArray &secret) Q_DECL_OVERRIDE;
    Sailfish::Secrets::Result getSecret(const QString &collectionName, const QString &secretName, QByteArray *secret) Q_DECL_OVERRIDE;
    Sailfish::Secrets::Result removeSecret(const QString &collectionName, const QString &secretName) Q_DECL_OVERRIDE;

    Sailfish::Secrets::Result reencryptSecrets(
            const QString &collectionName,          // non-empty, all secrets in this collection will be re-encrypted
            const QVector<QString> &secretNames,    // if collectionName is empty, these standalone secrets will be re-encrypted.
            const QByteArray &oldkey,
            const QByteArray &newkey,
            Sailfish::Secrets::EncryptionPlugin *plugin) Q_DECL_OVERRIDE;

private:
    class DatabaseLocker : public QMutexLocker
    {
    public:
        DatabaseLocker(Sailfish::Secrets::Daemon::Plugins::Sqlite::Database *db)
            : QMutexLocker(db->withinTransaction() ? Q_NULLPTR : db->accessMutex())
            , m_db(db) {}
        ~DatabaseLocker();
    private:
        Sailfish::Secrets::Daemon::Plugins::Sqlite::Database *m_db;
    };
    Sailfish::Secrets::Daemon::Plugins::Sqlite::Database *m_db;
};

} // namespace Plugins

} // namespace Daemon

} // namespace Secrets

} // namespace Sailfish

#endif // SAILFISHSECRETS_PLUGIN_STORAGE_SQLITE_H
