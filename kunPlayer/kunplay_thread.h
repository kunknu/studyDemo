#ifndef KUNPLAY_THREAD_H
#define KUNPLAY_THREAD_H

#include <QThread>
#include <QImage>

extern "C"
{
    #include "libavcodec/avcodec.h"
    #include "libavformat/avformat.h"
    #include <libavutil/time.h>
    #include "libavutil/pixfmt.h"
    #include "libswscale/swscale.h"
    #include "libswresample/swresample.h"

    #include <SDL.h>
    #include <SDL_audio.h>
    #include <SDL_types.h>
    #include <SDL_name.h>
    #include <SDL_main.h>
    #include <SDL_config.h>
}

#include "kunplay_showvideowight.h"


typedef struct PacketQueue {
    AVPacketList *first_pkt,*last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 1
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

#define MAX_VIDEO_SIZE (25*256*1024)
#define MAX_AUDIO_SIZE (25*16*1024) //队列里缓存的最大数据大小

class kunplay_thread; //前置声明

typedef struct VideoState
{
    AVFormatContext *ic;
    int videoStream,audioStream;
    AVFrame *audio_frame;//
    PacketQueue audioq;
    AVStream *audio_st;
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    AVPacket audio_pkt;
    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    uint8_t *audio_buf;
    //形如 align(16) unsigned char audio_buf2[AVCODEC_MAX_AUDIO_FRAME_SIZE*4]
    DECLARE_ALIGNED(16,uint8_t,audio_buf2)[AVCODEC_MAX_AUDIO_FRAME_SIZE*4];//字节对齐用
    enum AVSampleFormat audio_src_fmt;
    enum AVSampleFormat audio_tgt_fmt;
    /*
    channels  为 音频的 通道数 1 2 3 4 5.....
    channel_layout  为音频 通道格式类型 如 单通道 双通道 .....
     */
    int audio_src_channels;
    int audio_tgt_channels;
    int64_t audio_src_channel_layout;
    int64_t audio_tgt_channel_layout;
    int audio_src_freq;
    int audio_tgt_freq;
    struct SwrContext *swr_ctx; //用于解码后的音频格式转换；
    int audio_hw_buf_size; //？？什么的大小；

    double audio_clock;
    double video_clock;

    AVStream *video_st;
    PacketQueue videoq;

    //跳转相关变量
    int seek_req;
    int64_t seek_pos;
    int seek_flag_audio;
    int seek_flag_video;
    double seek_time;

    //播放控制相关
    bool isNeedPause; //跳转标志
    bool isPause; //暂停标志
    bool quit;    //停止
    bool readFinished; //文件读取完毕
    bool readThreadFinished;
    bool videoThreadFinished;

    SDL_Thread *video_tid; //视频线程

    kunplay_thread *player;
    bool isMute;
    float mVolume; //0-1超过1表示放大倍速

}VideoState;

class kunplay_thread :public QThread
{
    Q_OBJECT

public:

    enum PlayState
    {
        playing,
        Pause,
        Stop
    };

    explicit kunplay_thread();
    ~kunplay_thread();

    bool setFileName(QString path);
    bool replay();//重新播放
    bool paly();
    bool pause();
    bool stop(bool isWait = false); //参数表示是否等待所有的线程执行完毕再返回
    void seek(int64_t pos); //单位是微秒
    void setMute(bool isMute){mIsMute = isMute;}
    void setVolume(float value);

    int64_t getTotalTime();//单位微妙
    double getCurrentTime();//微妙

    void disPlayVideo(QImage img);

    void setVideoWidget(kunplay_showvideowight* widget);
    QWidget *getVideWidght(){return mVideoWidget;}

signals:
    void sig_GetOneFrame(QImage);//每获取一帧图像 就发送此信号

    void sig_StateChanged(kunplay_thread::PlayState state);
    void sig_TotalTimeChanged(qint64 uSec);//获取到视频时长就激发此信号
protected:
    void run() override;


private:
    QString mFileName;
    VideoState mVideoState;
    PlayState mPlayState;//播放状态

    //使用qt的控件代替SDL，SDL会导致QSS样式失效
    kunplay_showvideowight* mVideoWidget;

    bool mIsMute;
    float mVolume;

    SDL_AudioDeviceID mAudioID;

    int openSDL();
    void closeSDL();
    void deInit();

};

#endif // KUNPLAY_THREAD_H
