#include "widget.h"
#include "ui_widget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QFileDialog>
#include <QDebug>
#include <QDesktopWidget>
#include <QFontDatabase>
#include <QMouseEvent>

#define MARGINS 2 //窗体边框


Q_DECLARE_METATYPE(kunplay_thread ::PlayState)//自定义数据类型


Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);


}

Widget::~Widget()
{
    delete ui;
}

