#include "kunplay_showvideowight.h"
#include "ui_kunplay_showvideowight.h"
#include<QPainter>

kunplay_showvideowight::kunplay_showvideowight(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::kunplay_showvideowight)
{
    ui->setupUi(this);
}

kunplay_showvideowight::~kunplay_showvideowight()
{
    delete ui;
}

void kunplay_showvideowight::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    painter.setRenderHint(QPainter::Antialiasing);//抗锯齿
    painter.setRenderHint(QPainter::TextAntialiasing);//字体抗锯齿
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::HighQualityAntialiasing);

    painter.setBrush(Qt::black);
    painter.drawRect(0,0,this->width(),this->height());

    if(mImage.size().width()<=0) return;

    QImage img =mImage.scaled(this->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);
    int x=this->width()-img.width();
    int y=this->height()-img.height();
    x/=2;
    y/=2;

    painter.drawImage(QPoint(x,y),img);
}

void kunplay_showvideowight::slotGetOneFrame(QImage img)
{
    mImage=img;
    update();
}
