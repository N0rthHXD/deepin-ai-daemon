// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "embedding.h"
#include "vectorindex.h"
#include "database/embeddatabase.h"
#include "../global_define.h"
#include "utils/utils.h"

#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDebug>
#include <QDir>
#include <QtConcurrent/QtConcurrent>

#include <docparser.h>

static constexpr char kSearchResultDistance[] { "distance" };

Embedding::Embedding(QSqlDatabase *db, QMutex *mtx, const QString &appID, QObject *parent)
    : QObject(parent)
    , dataBase(db)
    , dbMtx(mtx)
    , appID(appID)
{
    Q_ASSERT(db);
    Q_ASSERT(mtx);
}

bool Embedding::embeddingDocument(const QString &docFilePath)
{
    QFileInfo docFile(docFilePath);
    if (!docFile.exists()) {
        qWarning() << docFilePath << "not exist";
        return false;
    }

    if (isDupDocument(docFilePath)) {
        qWarning() << docFilePath << "dump doc duplicate";
        return false;
    }

    for (auto id : embedDataCache.keys()) {
        if (docFilePath == embedDataCache.value(id).first) {
            qWarning() << docFilePath << "cache doc duplicate";
            return false;
        }
    }

    std::string stdStrContents = DocParser::convertFile(docFilePath.toStdString());
    QString contents = Utils::textEncodingTransferUTF8(stdStrContents);

    if (!Utils::isValidContent(stdStrContents)) {
        qDebug() << "Invalid document content.";
        return false;
    }

    //文本分块
    QStringList chunks;
    if (!contents.isEmpty())
        chunks = textsSpliter(contents);

    // 文件名大于14字节建索引
    if (docFile.baseName().toUtf8().size() > 14) {
        chunks.prepend(docFile.fileName());
    }

    if (chunks.isEmpty())
        return false;

    qDebug() << "embedding " << docFilePath << chunks.size();
    // 只需前100个
    if (chunks.size() > 100) {
        chunks = chunks.mid(0, 100);
        qDebug() << "Get the top 100 chunks" << docFilePath;
    }

    //向量化文本块，生成向量vector
    QVector<QVector<float>> vectors;
    vectors = embeddingTexts(chunks);

    if (vectors.count() != chunks.count())
        return false;
    if (vectors.isEmpty())
        return false;

    {
        QMutexLocker lk(&embeddingMutex);
        //元数据、文本存储
        int continueID = embedDataCache.size() + getDBLastID();
        qInfo() << "-------------" << continueID;

        for (int i = 0; i < chunks.count(); i++) {
            if (chunks[i].isEmpty())
                continue;

            embedDataCache.insert(continueID, QPair<QString, QString>(docFilePath, chunks[i]));
            embedVectorCache.insert(continueID, vectors[i]);

            continueID += 1;
        }
    }
    return true;
}

bool Embedding::embeddingDocumentSaveAs(const QString &docFilePath)
{
    // Embedding SaveAs
    // uos-ai
    QFileInfo docFile(docFilePath);
    if (!docFile.exists()) {
        qWarning() << docFilePath << "not exist";
        return false;
    }

    QString newDocPath = saveAsDocPath(docFilePath);

    if (isDupDocument(newDocPath)) {
        qWarning() << newDocPath << "dump doc duplicate";
        return false;
    }

    for (auto id : embedDataCache.keys()) {
        if (newDocPath == embedDataCache.value(id).first) {
            qWarning() << newDocPath << "cache doc duplicate";
            return false;
        }
    }

    std::string stdStrContents = DocParser::convertFile(docFilePath.toStdString());
    QString contents = Utils::textEncodingTransferUTF8(stdStrContents);

    if (!Utils::isValidContent(stdStrContents)) {
        qDebug() << "Invalid document content.";
        return false;
    }

    if (contents.isEmpty())
        return false;
    qInfo() << "embedding " << newDocPath;

    //文本分块
    QStringList chunks;
    chunks = textsSpliter(contents);
    //向量化文本块，生成向量vector
    QVector<QVector<float>> vectors;
    vectors = embeddingTexts(chunks);

    if (vectors.count() != chunks.count())
        return false;
    if (vectors.isEmpty())
        return false;

    {
        QMutexLocker lk(&embeddingMutex);
        //元数据、文本存储
        int continueID = embedDataCache.size() + getDBLastID();
        qInfo() << "-------------" << continueID;

        for (int i = 0; i < chunks.count(); i++) {
            if (chunks[i].isEmpty())
                continue;

            embedDataCache.insert(continueID, QPair<QString, QString>(newDocPath, chunks[i]));
            embedVectorCache.insert(continueID, vectors[i]);

            continueID += 1;
        }
    }

    return true;
}

QVector<QVector<float>> Embedding::embeddingTexts(const QStringList &texts)
{
    if (texts.isEmpty())
        return {};

    int inputBatch = 15;
    QVector<QVector<float>> vectors;
    QStringList splitProcessText = texts;
    int currentIndex = 0;
    while (currentIndex < splitProcessText.size()) {
        QStringList subList = splitProcessText.mid(currentIndex, inputBatch);
        currentIndex += inputBatch;

        QJsonObject emdObject = onHttpEmbedding(subList, apiData);
        QJsonArray embeddingsArray = emdObject["data"].toArray();
        for(auto embeddingObject : embeddingsArray) {
            QJsonArray vectorArray = embeddingObject.toObject()["embedding"].toArray();
            QVector<float> vectorTmp;
            for (auto value : vectorArray) {
                vectorTmp << static_cast<float>(value.toDouble());
            }
            vectors << vectorTmp;
        }
    }
    return vectors;
}

void Embedding::embeddingQuery(const QString &query, QVector<float> &queryVector)
{
    /* query:查询问题
     * queryVector:问题向量化
     *
     * 调用接口将query进行向量化，结果通过queryVector传递float指针
    */

    QStringList queryTexts;
    queryTexts << "为这个句子生成表示以用于检索相关文章:" + query;
    QJsonObject emdObject;
    emdObject = onHttpEmbedding(queryTexts, apiData);

    //获取query
    //local
    QJsonArray embeddingsArray = emdObject["data"].toArray();
    for(auto embeddingObject : embeddingsArray) {
        QJsonArray vectorArray = embeddingObject.toObject()["embedding"].toArray();
        for (auto value : vectorArray) {
            queryVector << static_cast<float>(value.toDouble());
        }
    }
}

bool Embedding::batchInsertDataToDB(const QStringList &inserQuery)
{
    if (inserQuery.isEmpty())
        return false;

    QMutexLocker lk(dbMtx);
    bool ok = EmbedDBVendorIns->commitTransaction(dataBase, inserQuery);
    return ok;
}

int Embedding::getDBLastID()
{
    QList<QVariantList> result;

    {
        QString query = "SELECT id FROM " + QString(kEmbeddingDBIndexSegTable) + " ORDER BY id DESC LIMIT 1";
        QMutexLocker lk(dbMtx);
        EmbedDBVendorIns->executeQuery(dataBase, query, result);
    }

    if (result.isEmpty())
        return 0;

    if (!result[0][0].isValid())
        return 0;

    return result[0][0].toInt() + 1;
}

void Embedding::createEmbedDataTable()
{
    qInfo() << "create DB table *****";

    QString createTable1SQL = "CREATE TABLE IF NOT EXISTS " + QString(kEmbeddingDBMetaDataTable) + " (id INTEGER PRIMARY KEY, source TEXT, content TEXT)";
    QString createTable2SQL = "CREATE TABLE IF NOT EXISTS " + QString(kEmbeddingDBIndexSegTable) + " (id INTEGER PRIMARY KEY, deleteBit INTEGER, content TEXT)";

    QMutexLocker lk(dbMtx);
    EmbedDBVendorIns->executeQuery(dataBase, createTable1SQL);
    EmbedDBVendorIns->executeQuery(dataBase, createTable2SQL);
    return ;
}

bool Embedding::isDupDocument(const QString &docFilePath)
{
    QList<QVariantList> result;

    QString query = "SELECT CASE WHEN EXISTS (SELECT 1 FROM " + QString(kEmbeddingDBMetaDataTable)
            + " WHERE source = '" + docFilePath + "') THEN 1 ELSE 0 END";

    {
        QMutexLocker lk(dbMtx);
        EmbedDBVendorIns->executeQuery(dataBase, query, result);
    }

    if (result.isEmpty())
        return 0;
    if (!result[0][0].isValid())
        return 0;
    return result[0][0].toBool();
}

void Embedding::embeddingClear()
{
    embedDataCache.clear();
    embedVectorCache.clear();
}

QMap<faiss::idx_t, QVector<float> > Embedding::getEmbedVectorCache()
{
    QMutexLocker lk(&embeddingMutex);
    return embedVectorCache;
}

QMap<faiss::idx_t, QPair<QString, QString>> Embedding::getEmbedDataCache()
{
    QMutexLocker lk(&embeddingMutex);
    return embedDataCache;
}

QStringList Embedding::textsSpliter(QString &texts)
{
    QStringList chunks;
    QStringList splitTexts;

    QRegularExpression regexSplit("[\n，；。,.]");
    QRegularExpression regexInvalidChar("[\\s\u200B]+");
    texts.replace(regexInvalidChar, " ");
    texts.replace("'", "\""); //SQL语句有单引号会报错

    splitTexts = texts.split(regexSplit, QString::SplitBehavior::SkipEmptyParts);

    QString over = "";
    for (auto text : splitTexts) {
        text = over + text;
        over = "";

        if (text.length() > kMaxChunksSize) {
            textsSplitSize(text, chunks, over);
        } else if (text.length() > kMinChunksSize && text.length() < kMaxChunksSize) {
            chunks << text;
        } else {
            over = text;
        }
    }

    if (over.length() > kMinChunksSize)
        chunks << over;
    else {
        if (chunks.isEmpty())
            chunks << over;
        else
            chunks.last() += over;
    }

    return chunks;
}

void Embedding::textsSplitSize(const QString &text, QStringList &splits, QString &over, int pos)
{
    if (pos >= text.length()) {
        return;
    }

    QString part = text.mid(pos, kMaxChunksSize);

    if (part.length() < kMaxChunksSize) {
        over += part;
        return;
    }
    splits << part;
    textsSplitSize(text, splits, over, pos + kMaxChunksSize);
}

QPair<QString, QString> Embedding::getDataCacheFromID(const faiss::idx_t &id)
{
    QMutexLocker lk(&embeddingMutex);
    return QPair<QString, QString>(embedDataCache[id].first, embedDataCache[id].second);
}

QString Embedding::saveAsDocPath(const QString &doc)
{
    QString docDirStr = workerDir() + QDir::separator() + appID + QDir::separator() + "Docs";
    QDir docDir(docDirStr);
    if (!docDir.exists()) {
        if (!docDir.mkpath(docDirStr)) {
            qWarning() << appID << " directory isn't exists and can't create!";
            return "";
        }
    }

    return docDirStr + QDir::separator() + QFileInfo(doc).fileName();
}

QString Embedding::loadTextsFromSearch(int topK, const QMap<float, faiss::idx_t> &cacheSearchRes, const QMap<float, faiss::idx_t> &dumpSearchRes)
{
    QJsonObject resultObj;
    resultObj["version"] = SEARCH_RESULT_VERSION;
    QJsonArray resultArray;

    if (appID == kSystemAssistantKey) {
        for (auto dumpIt : dumpSearchRes.keys()) {
            faiss::idx_t id = dumpSearchRes.value(dumpIt);
            QList<QVariantList> result;

            QString query = "SELECT * FROM " + QString(kEmbeddingDBMetaDataTable) + " WHERE id = " + QString::number(id);
            {
                QMutexLocker lk(dbMtx);
                EmbedDBVendorIns->executeQuery(dataBase, query, result);
            }

            if (result.isEmpty()) {
                return {};
            }

            QVariantList &res = result[0];
            if (!res[1].isValid() || !res[2].isValid())
                continue;

            QString source = res[1].toString();
            QString content = res[2].toString();
            QJsonObject obj;
            obj[kEmbeddingDBMetaDataTableSource] = source;
            obj[kEmbeddingDBMetaDataTableContent] = content;
            obj[kSearchResultDistance] = static_cast<double>(dumpIt);
            resultArray.append(obj);
        }
        resultObj["result"] = resultArray;
        return QJsonDocument(resultObj).toJson(QJsonDocument::Compact);
    }

    //TODO:重排序\合并两种结果；两个距离有序数组的合并
    int sort = 1;
    int i = 0;
    int j = 0;

    while (i < cacheSearchRes.size() && j < dumpSearchRes.size()) {
        if (sort > topK)
            break;

        auto cacheIt = cacheSearchRes.begin() + i;
        auto dumpIt = dumpSearchRes.begin() + j;
        if (cacheIt.key() < dumpIt.key()) {
            QString source = getDataCacheFromID(cacheIt.value()).first;
            QString content = getDataCacheFromID(cacheIt.value()).second;
            QJsonObject obj;
            obj[kEmbeddingDBMetaDataTableSource] = source;
            obj[kEmbeddingDBMetaDataTableContent] = content;
            obj[kSearchResultDistance] = static_cast<double>(cacheIt.key());
            resultArray.append(obj);
            sort++;
            i++;
        } else {
            faiss::idx_t id = dumpIt.value();
            QList<QVariantList> result;

            {
                QString query = "SELECT * FROM " + QString(kEmbeddingDBMetaDataTable) + " WHERE id = " + QString::number(id);
                QMutexLocker lk(dbMtx);
                EmbedDBVendorIns->executeQuery(dataBase, query, result);
            }

            if (result.isEmpty()) {
                j++;
                continue;
            }

            QVariantList &res = result[0];
            if (!res[1].isValid() || !res[2].isValid())
                continue;

            QString source = res[1].toString();
            QString content = res[2].toString();
            QJsonObject obj;
            obj[kEmbeddingDBMetaDataTableSource] = source;
            obj[kEmbeddingDBMetaDataTableContent] = content;
            obj[kSearchResultDistance] = static_cast<double>(dumpIt.key());
            resultArray.append(obj);
            sort++;
            j++;
        }
    }

    while (i < cacheSearchRes.size()) {
        if (sort > topK)
            break;

        auto cacheIt = cacheSearchRes.begin() + i;
        QString source = getDataCacheFromID(cacheIt.value()).first;
        QString content = getDataCacheFromID(cacheIt.value()).second;
        QJsonObject obj;
        obj[kEmbeddingDBMetaDataTableSource] = source;
        obj[kEmbeddingDBMetaDataTableContent] = content;
        obj[kSearchResultDistance] = static_cast<double>(cacheIt.key());
        resultArray.append(obj);
        sort++;
        i++;
    }

    while (j < dumpSearchRes.size()) {
        if (sort > topK)
            break;

        auto dumpIt = dumpSearchRes.begin() + j;
        faiss::idx_t id = dumpIt.value();
        QList<QVariantList> result;
        {
            QString query = "SELECT * FROM " + QString(kEmbeddingDBMetaDataTable) + " WHERE id = " + QString::number(id);
            QMutexLocker lk(dbMtx);
            EmbedDBVendorIns->executeQuery(dataBase, query, result);
        }
        if (result.isEmpty()) {
            j++;
            continue;
        }

        QVariantList &res = result[0];
        if (!res[1].isValid() || !res[2].isValid())
            continue;

        QString source = res[1].toString();
        QString content = res[2].toString();
        QJsonObject obj;
        obj[kEmbeddingDBMetaDataTableSource] = source;
        obj[kEmbeddingDBMetaDataTableContent] = content;
        obj[kSearchResultDistance] = static_cast<double>(dumpIt.key());
        resultArray.append(obj);
        sort++;
        j++;
    }
    resultObj["result"] = resultArray;
    qDebug() << QString::fromUtf8(QJsonDocument(resultObj).toJson(QJsonDocument::Compact));
    return QJsonDocument(resultObj).toJson(QJsonDocument::Compact);
}

void Embedding::deleteCacheIndex(const QStringList &files)
{
    if (files.isEmpty())
        return;

    QMutexLocker lk(&embeddingMutex);
    for (auto id : embedDataCache.keys()) {
        if (!files.contains(embedDataCache.value(id).first))
            continue;

        //删除缓存文档数据、删除向量
        embedDataCache.remove(id);
        embedVectorCache.remove(id);
    }
}

bool Embedding::doIndexDump(faiss::idx_t startID, faiss::idx_t endID)
{
    QMutexLocker lk(&embeddingMutex);
    //插入源信息
    QStringList insertSqlstrs;
    for (faiss::idx_t id = startID; id <= endID; id++) {
        if (!embedDataCache.contains(id))
            continue;

        QString queryStr = "INSERT INTO embedding_metadata (id, source, content) VALUES ("
                + QString::number(id) + ", '" + embedDataCache.value(id).first + "', " + "'" + embedDataCache.value(id).second + "')";
        insertSqlstrs << queryStr;

        embedDataCache.remove(id);
        embedVectorCache.remove(id);
    }

    if (insertSqlstrs.isEmpty())
        return false;

    if (!batchInsertDataToDB(insertSqlstrs)) {
        qWarning() << "Insert DB failed.";
        return false;
    }

    return true;
}

bool Embedding::doSaveAsDoc(const QString &file)
{
    QString newDocPath = saveAsDocPath(file);

    QProcess process;
    process.start("cp", QStringList() << file << newDocPath);
    process.waitForFinished();
    if (process.exitCode() == 0) {
        process.start("chmod", QStringList() << "444" << newDocPath);
        process.waitForFinished();
        if (process.exitCode() == 0) {
            qDebug() << "File copy successed.";
            return true;
        } else {
            qDebug() << "File copy failed.";
            // rm
            return false;
        }
    } else {
        qDebug() << "File copy failed.";
        return false;
    }
}

bool Embedding::doDeleteSaveAsDoc(const QStringList &files)
{
    for (const QString &oldDocPath : files) {
        QString docDirStr = workerDir() + QDir::separator() + appID + QDir::separator() + "Docs";
        QDir docDir(docDirStr);
        if (!docDir.exists()) {
            if (!docDir.mkpath(docDirStr)) {
                qWarning() << appID << " directory isn't exists and can't create!";
                return false;
            }
        }
        QString newDocPath = saveAsDocPath(oldDocPath);

        QProcess process;
        process.start("rm", QStringList() << newDocPath);
        process.waitForFinished();

        if (process.exitCode() == 0) {
            qDebug() << "File deleted successfully.";
        } else {
            qDebug() << "File delete failed.";
            return false;
        }
    }
    return true;
}
