// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "indexmanager.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QDebug>

IndexManager::IndexManager(QObject *parent)
    : QObject(parent),
      workThread(new QThread),
      worker(new IndexWorker),
      embeddingWorkThread(new QThread),
      embeddingWorker(new EmbeddingWorker)
{
    init();
}

IndexManager::~IndexManager()
{
    if (workThread->isRunning()) {
        worker->stop();
        //TODO EmbeddingWorker stop

        workThread->quit();
        workThread->wait();

        embeddingWorkThread->quit();
        embeddingWorkThread->wait();
    }

    qInfo() << "The index manager has quit";
    qInfo() << "The vector index manager has quit";
}

void IndexManager::init()
{
    //connect(this, &IndexManager::createAllIndex, worker.data(), &IndexWorker::onCreateAllIndex);
    connect(this, &IndexManager::fileCreated, worker.data(), &IndexWorker::onFileCreated);
    connect(this, &IndexManager::fileAttributeChanged, worker.data(), &IndexWorker::onFileAttributeChanged);
    connect(this, &IndexManager::fileDeleted, worker.data(), &IndexWorker::onFileDeleted);

    connect(this, &IndexManager::createAllIndex, embeddingWorker.data(), &EmbeddingWorker::onCreateAllIndex);
    connect(this, &IndexManager::docCreate, embeddingWorker.data(), &EmbeddingWorker::onDocCreate);
    connect(this, &IndexManager::docDelete, embeddingWorker.data(), &EmbeddingWorker::onDocDelete);

    workThread->start();
    embeddingWorkThread->start();

    embeddingWorker->moveToThread(embeddingWorkThread.data());
    worker->moveToThread(workThread.data());

    modelInit();
}

void IndexManager::modelInit()
{
    embeddingWorker->setEmbeddingApi(embeddingApi, this);

    if (ModelhubWrapper::isModelhubInstalled()) {
        if (!ModelhubWrapper::isModelInstalled(dependModel()))
            qWarning() << QString("VectorIndex needs model %0, but it is not avalilable").arg(dependModel());
    } else {
        qWarning() << "VectorIndex depends on deepin modehub, but it is not avalilable";
    }

    bgeModel = new ModelhubWrapper(dependModel(), this);
}

QJsonObject IndexManager::embeddingApi(const QStringList &texts, void *user)
{
    IndexManager *self = static_cast<IndexManager *>(user);
    if (!self->bgeModel->ensureRunning()) {
        return {};
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(self->bgeModel->urlPath("/embeddings"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonArray jsonArray;
    for (const QString &str : texts) {
        jsonArray.append(str);
    }
    QJsonValue jsonValue = QJsonValue(jsonArray);

    QJsonObject data;
    data["input"] = jsonValue;

    QJsonDocument jsonDocHttp(data);
    QByteArray jsonDataHttp = jsonDocHttp.toJson();
    QJsonDocument replyJson;
    QNetworkReply *reply = manager.post(request, jsonDataHttp);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray response = reply->readAll();
        replyJson = QJsonDocument::fromJson(response);
        qDebug() << "Response ok";
    } else {
        qDebug() << "Failed to create data:" << reply->errorString();
    }
    reply->deleteLater();
    QJsonObject obj = {};
    if (replyJson.isObject()) {
        obj = replyJson.object();
        return obj;
    }
    return {};
}
