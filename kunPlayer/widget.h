#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QImage>
#include <QPaintEvent>
#include <QTimer>
#include <QPushButton>

#include "kunplay_thread.h"

//鼠标实现改变窗口大小
#define PADDING 6
enum Direction { UP=0, DOWN, LEFT, RIGHT, LEFTTOP, LEFTBOTTOM, RIGHTBOTTOM, RIGHTTOP, NONE };



QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE


///这个是播放器的主界面 包括那些按钮和进度条之类的
class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = nullptr);
    ~Widget();

    void setTitle(QString str);

protected:
    void doClose();

private:
    Ui::Widget *ui;

    kunplay_thread *mplayer; //播放线程
    QTimer *mTimer; //定时器-获取当前视频时间

private slots:
    ///播放器相关的槽函数
    void slotTotalTimeChanged(qint64 uSec);
    void slotSliderMoved(int value);
    void slotTimerTimeOut();
    void slotBtnClick();

    void slotStateChanged(kunplay_thread::PlayState state);


    ///以下是改变窗体大小相关
        ////////
protected:
//    bool eventFilter(QObject *obj, QEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void mousePressEvent(QMouseEvent *event);

private:
    bool isMax; //是否最大化
    QRect mLocation;

    bool isLeftPressDown;  // 判断左键是否按下
    QPoint dragPosition;   // 窗口移动拖动时需要记住的点
    int dir;        // 窗口大小改变时，记录改变方向

    void checkCursorDirect(const QPoint &cursorGlobalPoint);//什么当前的方向

    void doShowFullScreen();//全屏
    void doShowNormal(); //普通大小

    void showBorderRadius(bool isShow);//什么边缘线
    void doChangeFullScreen(); //全屏改变？

private slots:
    void on_btnMenu_Close_clicked(); //菜单关闭？
    void on_btnMenu_Max_clicked(); //
    void on_btnMenu_Min_clicked();
};
#endif // WIDGET_H
