#include "kunplay_thread.h"

#include <stdio.h>
#include <QDebug>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

#define FLUSH_DATA "FLUSH"


static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;
    SDL_LockMutex(q->mutex);
    for(pkt=q->first_pkt;pkt!=NULL;pkt=pkt1)
    {
        pkt1=pkt->next;
        if(pkt1->pkt.data!=(uint8_t*)"FLUSH")
        {

        }
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt=NULL;
    q->first_pkt=NULL;
    q->nb_packets=0;
    q->size=0;
    SDL_UnlockMutex(q->mutex);
}

void packet_queue_deinit(PacketQueue *q){
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

void packet_queue_init(PacketQueue *q){
    memset(q,0,sizeof(PacketQueue));
    q->mutex=SDL_CreateMutex();
    q->cond=SDL_CreateCond();
    q->size=0;
    q->nb_packets=0;
    q->first_pkt=NULL;
    q->last_pkt=NULL;
}

int packet_queue_put(PacketQueue*q,AVPacket *pkt){

   AVPacketList *pkt1;
   if(av_dup_packet(pkt)<0){
       return -1;
   }
   pkt1 = (AVPacketList*)av_malloc(sizeof(AVPacket));
   if(!pkt1)
       return -1;
   pkt1->pkt=*pkt;
   pkt1->next=NULL;
   SDL_LockMutex(q->mutex);
   if(!q->last_pkt)//为空，则为第一个进队列
       q->first_pkt=pkt1;
   else
       q->last_pkt=pkt1;
   q->last_pkt=pkt1;
   q->size+=pkt1->pkt.size;
   q->nb_packets++;
   SDL_CondSignal(q->cond);
   SDL_UnlockMutex(q->mutex);
   return 0;
}
/*
 *
 * SDL_CondSignal: Use this function to restart one of the threads that are waiting on the condition variable.

  使用该函数来重启等待此信号的线程。

  SDL_CondWait: Use this function to wait until a condition variable is signaled.

  直到有该信号发送，调用此函数。

 */

static int packet_queue_get(PacketQueue* q,AVPacket *pkt ,int block)
{//这个block的作用是如果队列里面为空就一直取不会break，不然娶不到就break
    AVPacketList *pkt1;
    int ret;
    SDL_LockMutex(q->mutex);

    for(;;){

        pkt1=q->first_pkt;
        if(pkt1){
            q->first_pkt=pkt1->next;
            if(!q->first_pkt)
                q->last_pkt=NULL;
            q->nb_packets--;
            q->size-=pkt1->pkt.size;
            *pkt=pkt1->pkt;
            av_free(pkt1);
            ret=1;
            break;
        }else if(!block){
            ret=1;
            break;
        }else{
            SDL_CondWait(q->cond,q->mutex);
        }

    }
    SDL_CondWait(q->cond,q->mutex);
    return ret;
}

static int audio_decode_frame(VideoState *is,double *pts_ptr)
{
    int len1,len2,decode_data_size;
    AVPacket *pkt = &is->audio_pkt;
    int got_frame=0;
    int64_t dec_channel_layout;
    int wanted_nb_samles,resampled_data_size,n; //ressampled重采样数据大小
    double pts;

    for(;;){
        while (is->audio_buf_size>0){
            if(is->isPause==true)//判断暂停
            {
                return -1;//返回-1我是不理解的
                SDL_Delay(10);
                continue;
            }

            if(!is->audio_frame){//判断状态里面的帧类型为空就报错
                if(!(is->audio_frame=avcodec_alloc_frame())){
                    return AVERROR(ENOMEM);
                }
            }else//不为空
                avcodec_get_frame_defaults(is->audio_frame);
            len1=avcodec_decode_audio4(is->audio_st->codec,is->audio_frame,&got_frame,pkt);
            if(len1<0){//解码的长度小于0代表解完了,音频队列size置零
                is->audio_pkt_size=0;
                break;
            }
            is->audio_pkt_data +=len1;
            is->audio_pkt_size -=len1;
            if(!got_frame)
                continue;
            //计算解码出来的帧需要的大小
            decode_data_size = av_samples_get_buffer_size(NULL,
                                                          is->audio_frame->channels,
                                                          is->audio_frame->nb_samples,
                                                          (AVSampleFormat)is->audio_frame->format,1);

            dec_channel_layout = (is->audio_frame && is->audio_frame->channel_layout
                                  == av_get_channel_layout_nb_channels(is->audio_frame->channel_layout)?
                                      is->audio_frame->channel_layout:
                                      av_get_default_channel_layout(is->audio_frame->channels));

            wanted_nb_samles = is->audio_frame->nb_samples;

            //如果计算出来的采样率和想要的参数不一样 就进入
            if(is->audio_frame->format!=is->audio_src_fmt
                    ||dec_channel_layout!=is->audio_src_channel_layout
                    ||is->audio_frame->sample_rate!=is->audio_src_freq
                    ||(wanted_nb_samles!=is->audio_frame->nb_samples
                       &&!is->swr_ctx))//这个swr_ctr//用于解码后的音频格式转换；
            {
                //初始化音频转换的采样率的参数swr
                if(is->swr_ctx)
                    swr_free(&is->swr_ctx);
                is->swr_ctx=swr_alloc_set_opts(NULL,
                                               is->audio_tgt_channel_layout,(AVSampleFormat)is->audio_tgt_fmt,
                                               is->audio_tgt_freq,dec_channel_layout,
                                               (AVSampleFormat)is->audio_frame->format,is->audio_frame->sample_rate,
                                               0,NULL);
                if(!is->swr_ctx||swr_init(is->swr_ctx)<0)
                {
                    fprintf(stderr,"swr_init() failed\n");
                    break;
                }
                //这四次赋值暂时不知道是干嘛的
                is->audio_src_channel_layout=dec_channel_layout;
                is->audio_src_channels=is->audio_st->codec->channels;
                is->audio_src_freq=is->audio_st->codec->sample_rate;
                is->audio_src_fmt=is->audio_st->codec->sample_fmt;
            }

            /* 这里我们可以对采样数进行调整，增加或者减少，一般可以用来做声画同步 */
            if(is->swr_ctx)
            {
                //声明一个二维指针指向数据地址
                const uint8_t **in = (const uint8_t**)is->audio_frame->extended_data;
                //对齐字节用的输出数组？
                uint8_t *out[]={is->audio_buf2};
                //采样率转换
                if(wanted_nb_samles!=is->audio_frame->nb_samples)
                {
                     if(swr_set_compensation(is->swr_ctx,
                                             (wanted_nb_samles-is->audio_frame->nb_samples)
                                             *is->audio_tgt_freq
                                             /is->audio_frame->sample_rate,
                                             wanted_nb_samles*is->audio_tgt_freq
                                             /is->audio_frame->sample_rate)<0);
                     fprintf(stderr,"swr_set_co,pensation() fail\n");
                     break;
                }
                //swr转换？
                len2 = swr_convert(is->swr_ctx,out,
                                   sizeof (is->audio_buf2)/is->audio_tgt_channels
                                   /av_get_bytes_per_sample(is->audio_tgt_fmt),
                                   in,is->audio_frame->nb_samples);
                if(len2<0){
                    fprintf(stderr,"swr_convert() fail\n");
                    break;
                }
                //如果转换的大小==对齐字节的数组大小
                if(len2 == sizeof(is->audio_buf2)/is->audio_tgt_channels
                                   /av_get_bytes_per_sample(is->audio_tgt_fmt))
                {
                    //为什么要初始化
                    swr_init(is->swr_ctx);
                }
                is->audio_buf = is->audio_buf2;
                resampled_data_size = len2 * is->audio_tgt_channels * av_get_bytes_per_sample(is->audio_tgt_fmt);

            }
            else //如果swr为空则不需要转换采样率，能直接赋值音频数据，声卡决定需要的采样率
            {
                resampled_data_size = decode_data_size;
                is->audio_buf = is -> audio_frame->data[0];
            }
            pts = is->audio_clock;
            *pts_ptr = pts;
            n = 2*is->audio_st->codec->channels;
            is->audio_clock += (double)resampled_data_size / (double) (n * is->audio_st->codec->sample_rate);

            if(is->seek_flag_audio)
            {
               //发生了跳转 则跳过关键帧到目的时间的这几帧
                if(is->audio_clock<is->seek_time){
                    break;
                }
                else
                {//如果大于或者等于则把这个变量置0，为什么？
                    is->seek_flag_audio=0;
                }
            }
            return resampled_data_size;
        }
        if(pkt->data)
            av_free_packet(pkt);
        //释放了，为什么还要置零，这个不是个指针吗，应该相当于 =NULL；拉裤子放屁
        memset(pkt,0,sizeof (*pkt));
        if(is->quit)
        {
            packet_queue_flush(&is->audioq);
            return -1;
        }
        //直接退出函数，逻辑上不正常
        if(is->isPause==true)
        {
            return -1;
            //SDL_Delay(10);
            //continue;
        }
        //收到这个数据 说明刚刚执行过跳转 现在需要把解码器的数据 清除一下，可以测试一下如果不清空会怎么样
        if(strcmp((char*)pkt->data,FLUSH_DATA)==0)
        {
            avcodec_flush_buffers(is->audio_st->codec);
            av_free_packet(pkt);
            continue;
        }
        //为什么保存这一帧的数据？？？
        is->audio_pkt_data=pkt->data;
        is->audio_pkt_size=pkt->size;
        //这个时间同步得好看看一下？？？
         /* if update, update the audio clock w/pts */
        if(pkt->pts!=AV_NOPTS_VALUE){
            is->audio_clock= av_q2d(is->audio_st->time_base) *pkt->pts;
        }
    }
    return 0;
}
typedef     signed char         int8_t;
typedef     signed short        int16_t;
typedef     signed int          int32_t;
typedef     unsigned char       uint8_t;
typedef     unsigned short      uint16_t;
typedef     unsigned int        uint32_t;
typedef unsigned long       DWORD;
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef float               FLOAT;
typedef FLOAT               *PFLOAT;
typedef int                 INT;
typedef unsigned int        UINT;
typedef unsigned int        *PUINT;

typedef unsigned long ULONG_PTR, *PULONG_PTR;
typedef ULONG_PTR DWORD_PTR, *PDWORD_PTR;


//makeword是将两个byte型合并成一个word型，一个在高8位(b)，一个在低8位(a) 前面的是低8位，后面的是高8位所以左移8位 与255既1111...变成16位方便或。
#define MAKEWORD(a, b)      ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
//两个字节 成一个long 双字节
#define MAKELONG(a, b)      ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
//下面反同理，高位变低位
#define LOWORD(l)           ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l)           ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w)           ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w)           ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))

//调大音量
//buf为需要调节音量的音频数据块首地址指针，size为长度，uRepeat为重复次数，通常为1，vol为增益倍数，可以小于1
void RaiseVolume(char* buf ,int size ,int uRepeat ,double vol)
{
    if(!size){
        return ;
    }
    for(int i=0;i<size;i+=2)
    {
        short wData;
        wData = MAKEWORD(buf[i],buf[i+1]);//合成一个字
        long dwData=wData;//用两个字装防止溢出
        for(int i=0;i<size ;i+=2)
        {
            dwData *=vol;//乘以倍数
            if(dwData<-0x8000) //32768 short的下限
            {
                dwData=-0x8000;
            }
            else if(dwData>0x7FFF)//字的上限
            {
                dwData=0x7FFF;
            }
        }
        wData = LOWORD(dwData); //双字变单字，取低字节
        buf[i] = LOBYTE(wData); //低字节
        buf[i+1] = HIBYTE(wData);//高字节
    }
}


static void audio_callback(void *userdata,Uint8 *stream, int len)
{
    VideoState *is=(VideoState*)userdata;

    int len1, audio_data_size;

    double pts;
    //qDebug()<<__FUNCTION__<<"111...";
        /*   len是由SDL传入的SDL缓冲区的大小，如果这个缓冲未满，我们就一直往里填充数据 */
    while(len >0)//len是由SDL传入的SDL缓冲区的大小
    {
        /*  audio_buf_index 和 audio_buf_size 标示我们自己用来放置解码出来的数据的缓冲区，*/
        /*   这些数据待copy到SDL缓冲区， 当audio_buf_index >= audio_buf_size的时候意味着我*/
        /*   们的缓冲为空，没有数据可供copy，这时候需要调用audio_decode_frame来解码出更
         /*   多的桢数据 */
//        qDebug()<<__FUNCTION__<<is->audio_buf_index<<is->audio_buf_size;

        //audio_buf_size是指解码出来的大小 audio_buf_index是指每次拷贝进stream的大小
        //这两个变量属于上一次循环，一般都是等于，之所以有小于是因为，可能SDL的缓存满了，一帧数据存了部分
        //所以留着一般等着下一次循环送进SDL缓存
        if(is->audio_buf_index >= is->audio_buf_size)
        {
            audio_data_size = audio_decode_frame(is,&pts);//pts为解码出来的播放时间

            if(audio_data_size<0)//解码大小小于0，解码完毕
            {
                is->audio_buf_size=1024;    //一帧1024
                if(is->audio_buf==NULL) return; //空返回NULL
                memset(is->audio_buf,0,is->audio_buf_size); //不为空就初始化为0
            }
            else{ //如果解码大小大于0，大结构体的buf_size等于解码大小
                is->audio_buf_size = audio_data_size;
            }
            is->audio_buf_index=0; //重新一帧，以送入SDL缓存的大小置零
        }
        /*  查看stream可用空间，决定一次copy多少数据，剩下的下次继续copy */

        len1 = is->audio_buf_size - is->audio_buf_index;

        if(len1>len)//如果len1大于缓冲区剩余len大小
            len1=len; //直接赋值
        if(is->isMute || is->isNeedPause)//静音或者需要暂停，初始化内存为0
        {
            memset(is->audio_buf + is->audio_buf_index, 0, len1);
        }
        else//不然就调节音量大小
        {
            RaiseVolume((char*)is->audio_buf + is->audio_buf_index,len1,1,is->mVolume);
        }
        //关键函数，把数据送入SDL的缓存之中
        memcpy(stream,(uint8_t*)is->audio_buf+is->audio_buf_index,len1);//把结构体里的处理好的缓存拷贝到什么stream里
        len -=len1;
        stream+=len1;
        is->audio_buf_index+=len1;
    }
}

static double get_audio_clock(VideoState *is)
{
    double pts;
    int hw_buf_size, bytes_per_sec,n;
    /*  audio_buf_index 和 audio_buf_size 标示我们自己用来放置解码出来的数据的缓冲区，*/
    /*   这些数据待copy到SDL缓冲区， 当audio_buf_index >= audio_buf_size的时候意味着我*/
    /*   们的缓冲为空，没有数据可供copy，这时候需要调用audio_decode_frame来解码出更
     /*   多的桢数据 */
    pts = is->audio_clock;//这个值是哪里来的？double类型
    hw_buf_size = is->audio_buf_size - is->audio_buf_index; //上一帧还有多少数据大小还没送进SDL缓存
    bytes_per_sec=0;//多少bytes一秒
    n = is->audio_st->codec->channels * 2; //声道x2
    if(is->audio_st){
        bytes_per_sec = is->audio_st ->codec->sample_rate *n;//为什么码率要乘以上面的2
    }
    if(bytes_per_sec){//上面的执行成功不为0
        //通过 还剩多少数据没送进缓存大小/一秒的数据量 再用这一段的结尾时间减去这个值，就等于这小段数据当前的时间
        pts-= (double)hw_buf_size/bytes_per_sec;
    }
    return pts;
    //要留意 is->audio_clock 的赋值地方
}

























kunplay_thread::kunplay_thread()
{

}
