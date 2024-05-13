﻿// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vectorindex.h"
#include "../global_define.h"
#include "database/embeddatabase.h"

#include <QList>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFuture>
#include <QtConcurrent/QtConcurrent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>

#include <faiss/IndexIVFPQ.h>
#include <faiss/index_io.h>
#include <faiss/index_factory.h>
#include <faiss/utils/random.h>
#include <faiss/IndexShards.h>
#include <faiss/IndexFlatCodes.h>

VectorIndex::VectorIndex(QObject *parent)
    :QObject (parent)
{
    init();
}

void VectorIndex::init()
{
    connect(this, &VectorIndex::indexDump, this, &VectorIndex::onIndexDump);
}

bool VectorIndex::createIndex(int d, const QVector<float> &embeddings, const QVector<faiss::idx_t> &ids, const QString &indexKey)
{
//    //-----index factory
//    //QString ivfPqIndexKey = "IVF256, PQ8";
//    //index.reset(faiss::index_factory(d, flatIndexKey.toStdString().c_str()));

    /* n:向量个数
     * 一条1024dim，f32的向量大小为4KB，1w条40MB，检索时间为3ms左右；
     * 创建索引时按照n的大小(索引文件大小、检索时间)选择IndexType
     */
    int n = embeddings.count() / d;
    if (n != ids.count()) {
        qDebug() << "embedding error: vectors not equal ids.";
        return false;
    }

    if (n < 1000)
        //小于1000个向量，直接Flat
        return createFlatIndex(d, embeddings, ids, indexKey);
    else if (n < 1000000)
        return createIvfFlatIndex(d, embeddings, indexKey);

    return false;
}

bool VectorIndex::updateIndex(int d, const QVector<float> &embeddings, const QVector<faiss::idx_t> &ids, const QString &indexKey)
{
    if (embeddings.isEmpty())
        return false;    

    if (embeddings.count() / d != ids.count()) {
        qDebug() << "embedding error: vectors not equal ids.";
        return false;
    }

    if (!flatIndexHash.contains(indexKey)) {
        faiss::Index *index = faiss::index_factory(d, kFaissFlatIndex);
        faiss::IndexIDMap *flatIndexIDMap = new faiss::IndexIDMap(index);
        flatIndexHash.insert(indexKey, flatIndexIDMap);
    }
    faiss::IndexIDMap *flatIndexIDMapTmp = flatIndexHash.value(indexKey);

    int oldIndexTotal = flatIndexIDMapTmp->ntotal;
    int n = embeddings.count() / d - oldIndexTotal;
    QVector<float> embeddingsTmp = embeddings.mid(oldIndexTotal * d);
    QVector<faiss::idx_t> idsTmp = ids.mid(oldIndexTotal);
    flatIndexIDMapTmp->add_with_ids(n, embeddingsTmp.data(), idsTmp.data());

    segmentIds += idsTmp;   //每个segment的索引所对应的IDs

    if (flatIndexIDMapTmp->ntotal >= 2) {
        // UOS-AI添加文档后在内存中，与已经落盘的区分开，手动操作落盘；整理索引碎片等操作。
        Q_EMIT indexDump(indexKey);
    }
    return true;

    //TODO:会加载索引、消耗资源,存储信息到DB
//    int nTotal = getIndexNTotal(indexKey);

//    if (nTotal < 1000)
//        //小于1000个向量，直接Flat
//        return updateFlatIndex(d, embeddings, ids, indexKey);
//    else if (nTotal < 1000000) {
//        return updateIvfFlatIndex(d, embeddings, indexKey);
//    }


    //faiss::Index *newIndex = faiss::index_factory(d, "Flat");
    //faiss::IndexFlat *newIndex = new faiss::IndexFlat(d);
    //faiss::IndexFlat *newIndex1 = new faiss::IndexFlat(d);
    //faiss::idx_t n = ids.count();
//    faiss::IndexIDMap indexMap(index);
//    indexMap.add_with_ids(n, embeddings.data(), ids.data());
    //newIndex->add(n, embeddings.data());
    //newIndex1->add(n, embeddings.data());

    //faiss::IndexIDMapTemplate<faiss::Index> *newIndex = dynamic_cast<faiss::IndexIDMapTemplate<faiss::Index>*>(indexMap.index);
    //faiss::IndexFlatCodes *newIndex = dynamic_cast<faiss::IndexFlatCodes*>(index);

    //合并
    //faiss::idx_t aa1 = oldIndex->ntotal;
    //faiss::idx_t aa2 = indexMap.ntotal;
    //newIndex->merge_from(*oldIndex);
   // qInfo() << newIndex->ntotal;
    //oldIndex->merge_from(*indexMap.index);
    //faiss::idx_t aa = oldIndex->ntotal;

}

bool VectorIndex::deleteIndex(const QString &indexKey, const QVector<faiss::idx_t> &deleteID)
{
    return deleteFlatIndex(deleteID, indexKey);
}

bool VectorIndex::saveIndexToFile(const faiss::Index *index, const QString &indexKey, const QString &indexType)
{
    qInfo() << "save faiss index...";
    QString indexDirStr = workerDir() + QDir::separator() + indexKey;
    QDir indexDir(indexDirStr);

    if (!indexDir.exists()) {
        qWarning() << indexKey << " directory isn't exists and can't create!";
        return false;
    }
    QHash<QString, int> indexFilesNum = getIndexFilesNum(indexKey);
    QString indexName = indexType + "_" + QString::number(indexFilesNum.value(indexType) + 1) + ".faiss";
    QString indexPath = indexDir.path() + QDir::separator() + indexName;
    qInfo() << "index file save to " + indexPath;

    QStringList insertStrs;
    for (faiss::idx_t id : segmentIds) {
        QString insert = "INSERT INTO " + QString(kEmbeddingDBIndexSegTable)
                + " (id, " + QString(kEmbeddingDBSegIndexTableBitSet)
                + ", " + QString(kEmbeddingDBSegIndexIndexName) + ") " + "VALUES ("
                + QString::number(id) + ", " + "1" + ", '" + indexName + "')";
        insertStrs << insert;
    }
    QFuture<void> future =  QtConcurrent::run([indexKey, insertStrs](){
        QString query = "SELECT id FROM " + QString(kEmbeddingDBMetaDataTable) + " ORDER BY id DESC LIMIT 1";
        return EmbedDBManagerIns->commitTransaction(indexKey + ".db", insertStrs);
    });
    future.waitForFinished();

    segmentIds.clear();

    try {
        faiss::write_index(index, indexPath.toStdString().c_str());
        return true;
    } catch (faiss::FaissException &e) {
        std::cerr << "Faiss error: " << e.what() << std::endl;
    }

    return true;
}

faiss::Index* VectorIndex::loadIndexFromFile(const QString &indexKey, const QString &indexType)
{
    qInfo() << "load faiss index...";
    QString indexDirStr = workerDir() + QDir::separator() + indexKey;
    QDir indexDir(indexDirStr);

    if (!indexDir.exists()) {
        qWarning() << indexKey << " directory isn't exists!";
        return {};
    }
    QString indexPath = indexDir.path() + QDir::separator() + indexType + ".faiss";
    qInfo() << "load index file from " + indexPath;

    faiss::Index *index;
    try {
        index = faiss::read_index(indexPath.toStdString().c_str());
    } catch (faiss::FaissException &e) {
        std::cerr << "Faiss error: " << e.what() << std::endl;
        return nullptr;
    }
    return index;
}

QVector<faiss::idx_t> VectorIndex::vectorSearch(int topK, const float *queryVector, const QString &indexKey)
{
    //读取索引文件
    faiss::Index *index = loadIndexFromFile(indexKey, kFaissFlatIndex);
    if (!index)
        return {};
    //向量检索
    QVector<float> D1(topK);
    QVector<faiss::idx_t> I1(topK);
    index->search(1, queryVector, topK, D1.data(), I1.data());

    //TODO:检索结果后处理-去重、过于相近或远
//    QVector<faiss::idx_t> nonDupIndex;
//    QMap<float, bool> seen;
//    removeDupIndex(index, topK, 0, nonDupIndex, queryVector, seen);

    return I1;
}

void VectorIndex::onIndexDump(const QString &indexKey)
{
    saveIndexToFile(flatIndexHash.value(indexKey), indexKey, kFaissFlatIndex);

    flatIndexHash.remove(indexKey);
}

bool VectorIndex::createFlatIndex(int d, const QVector<float> &embeddings, const QVector<faiss::idx_t> &ids, const QString &indexKey)
{
    faiss::Index *index = faiss::index_factory(d, kFaissFlatIndex);
    faiss::idx_t n = embeddings.count() / d;

    faiss::IndexIDMap indexMap(index);
    indexMap.add_with_ids(n, embeddings.data(), ids.data());
    //index->add(n, embeddings.data());

    return saveIndexToFile(&indexMap, indexKey, kFaissFlatIndex);
}

bool VectorIndex::updateFlatIndex(int d, const QVector<float> &embeddings, const QVector<faiss::idx_t> &ids, const QString &indexKey)
{
    faiss::idx_t n = embeddings.count() / d;
    faiss::IndexIDMap *index = dynamic_cast<faiss::IndexIDMap*>(loadIndexFromFile(indexKey, kFaissFlatIndex));
    if (!index)
        return false;

    index->add_with_ids(n, embeddings.data(), ids.data());
    //index->add(n, embeddings.data());

    return saveIndexToFile(index, indexKey, kFaissFlatIndex);
}

bool VectorIndex::deleteFlatIndex(const QVector<faiss::idx_t> &deleteID, const QString &indexKey)
{
    faiss::IndexIDMap *index = dynamic_cast<faiss::IndexIDMap*>(loadIndexFromFile(indexKey, kFaissFlatIndex));
    if (!index)
        return false;
    faiss::IDSelectorArray idSelector(static_cast<size_t>(deleteID.size()), deleteID.data());
    index->remove_ids(idSelector);
    return saveIndexToFile(index, indexKey, kFaissFlatIndex);
}

bool VectorIndex::createIvfFlatIndex(int d, const QVector<float> &embeddings, const QString &indexKey)
{
    return false;
}

bool VectorIndex::updateIvfFlatIndex(int d, const QVector<float> &embeddings, const QString &indexKey)
{
    return false;
}

int VectorIndex::getIndexNTotal(const QString &indexKey)
{
    //获取当前索引中向量总数  XXX
    faiss::Index *index = loadIndexFromFile(indexKey, kFaissFlatIndex);
    if (!index)
        return -1;
    int nTotal = static_cast<int>(index->ntotal);

    return nTotal;
}

void VectorIndex::removeDupIndex(const faiss::Index *index, int topK, int DupK, QVector<faiss::idx_t> &nonDupIndex,
                                 const float *queryVector, QMap<float, bool> &seen)
{
    if (nonDupIndex.count() == topK)
        return;

    QVector<float> D1(topK);
    QVector<faiss::idx_t> I1(topK);
    index->search(1, queryVector, topK, D1.data(), I1.data());

    for (int i = 0; i < topK; i++) {
        if (!seen[D1[i]]) {
            if (nonDupIndex.count() == topK)
                return;
            seen[D1[i]] = true;
            nonDupIndex.push_back(I1[i]);
        }
    }
    DupK += topK - nonDupIndex.count();
    removeDupIndex(index, topK, DupK, nonDupIndex, queryVector, seen);
}

QHash<QString, int> VectorIndex::getIndexFilesNum(const QString &indexKey)
{
    QHash<QString, int> result;

    QString indexDirStr = workerDir() + QDir::separator() + indexKey;
    QDir indexDir(indexDirStr);
    if (!indexDir.exists()) {
        qWarning() << indexKey << " directory isn't exists!";
        return {};
    }

    QFileInfoList fileList = indexDir.entryInfoList(QDir::Files);

    for (QString indexTypeKey : {kFaissFlatIndex, kFaissIvfFlatIndex, kFaissIvfPQIndex}) {
        int count = 0;
        for (const QFileInfo& fileInfo : fileList) {
            QString fileName = fileInfo.fileName();
            if (fileName.contains(indexTypeKey))
                count += 1;
            result.insert(indexTypeKey, count);
        }
    }
    return result;
}
