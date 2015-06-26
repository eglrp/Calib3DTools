#ifndef POINT2DVIEW_H
#define POINT2DVIEW_H

#include <QObject>
#include <QTreeView>
#include "point2dmodel.h"
class Point2DView : public QTreeView
{
    Q_OBJECT
public:
    Point2DView(QWidget* parent=0);
    void setPoint2DModel(Point2DModel *point2DModel);
    Point2DModel *getPoint2DModel();
signals:
    void currentPointChanged(int indexImg,int indexPoint);
private slots:
    void onCurrentChanged(const QModelIndex & current, const QModelIndex & previous);
private:
    Point2DModel *point2DModel;
};

#endif // POINT2DVIEW_H
