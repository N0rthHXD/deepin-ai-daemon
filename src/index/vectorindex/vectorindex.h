// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VECTORINDEX_H
#define VECTORINDEX_H

#include <QObject>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QVector>

#include <faiss/Index.h>
#include <faiss/IndexFlat.h>
#include <faiss/IndexIDMap.h>

class VectorIndex : public QObject
{
    Q_OBJECT

public:
    explicit VectorIndex(QObject *parent = nullptr);
    void init();

    bool createIndex(int d, const QVector<float> &embeddings, const QVector<faiss::idx_t> &ids, const QString &indexKey);
    bool updateIndex(int d, const QVector<float> &embeddings, const QVector<faiss::idx_t> &ids, const QString &indexKey);
    bool deleteIndex(const QString &indexKey, const QVector<faiss::idx_t> &deleteID);
    bool saveIndexToFile(const faiss::Index *index, const QString &indexKey, const QString &indexType="All");
    faiss::Index* loadIndexFromFile(const QString &indexKey, const QString &indexType="All");

    //DB Operate
    void createIndexSegTable(const QString &key);

    QVector<faiss::idx_t> vectorSearch(int topK, const float *queryVector, const QString &indexKey);

    inline static QString workerDir()
    {
        static QString workerDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + "/embedding";
        return workerDir;
    }

signals:
    void indexDump(const QString &indexKey);

private slots:
    void onIndexDump(const QString &indexKey);

private:
    bool createFlatIndex(int d, const QVector<float> &embeddings, const QVector<faiss::idx_t> &ids, const QString &indexKey);
    bool updateFlatIndex(int d, const QVector<float> &embeddings, const QVector<faiss::idx_t> &ids, const QString &indexKey);
    bool deleteFlatIndex(const QVector<faiss::idx_t> &deleteID, const QString &indexKey);


    bool createIvfFlatIndex(int d, const QVector<float> &embeddings, const QString &indexKey);
    bool updateIvfFlatIndex(int d, const QVector<float> &embeddings, const QString &indexKey);

    int getIndexNTotal(const QString &indexKey);

    void removeDupIndex(const faiss::Index *index, int topK, int DupK, QVector<faiss::idx_t> &nonDupIndex,
                        const float *queryVector, QMap<float, bool> &seen);

    QHash<QString, int> getIndexFilesNum(const QString &indexKey);

    //faiss::IndexFlatL2 *flatIndex = nullptr;
    //faiss::IndexIDMap *flatIndexIDMap = nullptr;  //Flat L2 index, id mapping

    QHash<QString, faiss::IndexIDMap*> flatIndexHash;
    QVector<faiss::idx_t> segmentIds;
};

#endif // VECTORINDEX_H
