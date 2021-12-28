#ifndef KUNPLAY_SHOWVIDEOWIGHT_H
#define KUNPLAY_SHOWVIDEOWIGHT_H

#include <QWidget>

namespace Ui {
class kunplay_showvideowight;
}

class kunplay_showvideowight : public QWidget
{
    Q_OBJECT

public:
    explicit kunplay_showvideowight(QWidget *parent = nullptr);
    ~kunplay_showvideowight();

private:
    Ui::kunplay_showvideowight *ui;


public slots:
    void slotGetOneFrame(QImage img);
protected:
    void paintEvent(QPaintEvent *event);
    QImage mImage;

};

#endif // KUNPLAY_SHOWVIDEOWIGHT_H
