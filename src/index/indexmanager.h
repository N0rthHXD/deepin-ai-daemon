// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef INDEXMANAGER_H
#define INDEXMANAGER_H

#include "indexworker.h"
#include "index/embeddingworker.h"
#include "modelhub/modelhubwrapper.h"

#include <QObject>
#include <QThread>
#include <QSharedPointer>

class IndexManager : public QObject
{
    Q_OBJECT
public:
    explicit IndexManager(QObject *parent = nullptr);
    ~IndexManager();

    static inline QString dependModel() {
        return QString("BAAI-bge-large-zh-v1.5");
    }

Q_SIGNALS:
    void createAllIndex();
    void fileAttributeChanged(const QString &file);
    void fileCreated(const QString &file);
    void fileDeleted(const QString &file);

    void docCreate(const QString &doc);
    void docDelete(const QString &doc);

private:
    void init();
    void modelInit();

protected:
    static QJsonObject embeddingApi(const QStringList &texts, void *user);

private:
    QSharedPointer<QThread> workThread { nullptr };
    QSharedPointer<IndexWorker> worker { nullptr };

    QSharedPointer<QThread> embeddingWorkThread { nullptr };
    QSharedPointer<EmbeddingWorker> embeddingWorker { nullptr };
    ModelhubWrapper *bgeModel = nullptr;
};

#endif   // INDEXMANAGER_H
