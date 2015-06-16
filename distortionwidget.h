#ifndef DISTORTIONWIDGET_H
#define DISTORTIONWIDGET_H

#include <QWidget>
class QTableView;
class DistortionModel;
class DistortionWidget : public QWidget
{
    Q_OBJECT
public:
    explicit DistortionWidget(QWidget *parent = 0);
    void setModel(DistortionModel *model);
signals:

public slots:
    void saveFile();
    void loadFile();
    void clearDistortion();
private:
    bool saveDistortion(const QStringList& list);
    bool loadDistortion(const QStringList& list);
    QTableView *tableView;
    DistortionModel* model;
};

#endif // DISTORTIONWIDGET_H
