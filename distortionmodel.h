#ifndef DISTORTIONMODEL_H
#define DISTORTIONMODEL_H

#include <QtCore>
#include <QAbstractItemModel>
#include "distortion.h"

class DistortionModel : public QAbstractItemModel, public DistortionContainer
{
    Q_OBJECT
public:
    DistortionModel(QObject* parent=0);
    bool isEmpty();
    void makeEmpty();
    Distortion getDistortion_threadSafe();
    void saveDistortion_threadSafe(const Distortion& value);
    int rowCount(const QModelIndex & index=QModelIndex()) const;
    int columnCount(const QModelIndex &parent=QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role=Qt::DisplayRole) const;
    Qt::ItemFlags flags(const QModelIndex &index) const;
    bool setData(const QModelIndex &index, const QVariant &value, int role=Qt::DisplayRole);
    bool insertRows(int row, int count, const QModelIndex &parent);
    bool removeRows(int row, int count, const QModelIndex &parent);
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;
    QModelIndex index(int row, int column, const QModelIndex &parent) const;
    QModelIndex parent(const QModelIndex &child) const{return QModelIndex();}
signals:
    void requestGet();
    void requestSave(const Distortion& dist);
private slots:
    void prepareDistortion();
    void saveDistortion(const Distortion& dist);
private:

    //QList<double> para;
    Distortion modelData;
    QMutex mutex;
    QWaitCondition conditionGet;
    QWaitCondition conditionSave;
    Distortion preparedDistortion;//locked by mutex
};

#endif // DISTORTIONMODEL_H
