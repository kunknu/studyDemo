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
                if(is->audio_clock < is->seek_time){
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

//音频解码并送入SDL缓存中
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

//这个是音视频同步操作，
static double synchronize_video(VideoState *is,AVFrame *src_frame, double pts)
{
    double frame_delay;
    if(pts!=0){//若送进来的pts不为空则大结构体的video_clock为pts
        is->video_clock=pts;
    }else{ //反则反
        pts=is->video_clock;
    }//
    //根据时间基去转换成秒，时间基时间，还要加上延迟时间才算正确时间
    frame_delay = av_q2d(is->video_st->codec->time_base);
    /*
        当解码时，这个信号告诉你这张图片需要要延迟多少久。
        需要求出扩展延时：
        extra_delay = repeat_pict / (2*fps)
     */
    //正确的时间
    frame_delay += src_frame->repeat_pict * (frame_delay*0.5);
    //这个大结构体里的时间是0吗？为什么不=而用+=？
    is->video_clock += frame_delay;
    return pts;
}
//音频流组件打开？
int audio_stream_component_open(VideoState *is ,int stream_index)
{
    //is是自定义大结构体VideoState，ic是ffmpeg核心结构体AVFormatContext
    AVFormatContext *ic = is->ic;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    int64_t wanted_channel_layout =0 ;
    int wanted_nb_channels;

    if(stream_index < 0 || stream_index >= ic->nb_streams){
        return -1;
    }

    codecCtx = ic->streams[stream_index]->codec;
    wanted_nb_channels = codecCtx->channels;

    if(!wanted_channel_layout || wanted_nb_channels !=
            av_get_channel_layout_nb_channels(wanted_channel_layout))
    {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        //这个与上取反的操作不是很明白?
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;//立体混音
        //单从代码上来看，意思为：如果左右声道都有才为1
    }
    //把设置好的参数保存在大结构体里
    is->audio_src_fmt = is->audio_tgt_fmt = AV_SAMPLE_FMT_S16;
    is->audio_src_freq = is->audio_tgt_freq = 44100;
    is->audio_src_channel_layout = is->audio_tgt_channel_layout =wanted_channel_layout;
    is->audio_src_channels = is->audio_tgt_channels = 2;

    codec = avcodec_find_decoder(codecCtx->codec_id);
    if(!codec || (avcodec_open2(codecCtx,codec,NULL)<0)) //找不到解码器
    {
        fprintf(stderr,"Unsupported codec!\n");
        return -1;
    }
    //这个是抛弃帧数量变量
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (codecCtx->codec_type) { //这个类型应该必须的音频才能打开的，不懂为什么会判断有其他情况
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = ic->streams[stream_index];
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;
        memset(&is->audio_pkt, 0 , sizeof(is->audio_pkt));
        break;
    default:
        break;
    }
    return 0;
}

//核心视频线程
int video_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;

    int ret, got_picture,numBytes;

    double video_pts = 0; //当前视频的pts
    double audio_pts = 0; //音频pts

    //解码视频相关
    AVFrame *pFrame ,*pFrameRGB;
    uint8_t *out_buffer_rgb; //解码后的RGB数据
    struct SwsContext *img_convert_ctx; //用于解码后的格式转换结构体
    AVCodecContext *pCodecCtx = is->video_st->codec; //视频解码器
    pFrame = av_frame_alloc();
    pFrameRGB = av_frame_alloc();
    //YUV->RGB32
    //素拉伸的方式SWS_BICUBIC
//    SWS_BICUBIC性能比较好。
//    SWS_FAST_BILINEAR在性能和速度之间有一个比好好的平衡。
//    SWS_POINT的效果比较差。
    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
                                     pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
                                     PIX_FMT_BGR32, SWS_BICUBIC, NULL,NULL,NULL);
    //根据视频的格式得到一帧的大小
    numBytes = avpicture_get_size(PIX_FMT_BGR32, pCodecCtx->width, pCodecCtx->height);

    out_buffer_rgb = (uint8_t*) av_malloc(numBytes* sizeof(uint8_t));
    //根据指定的图像参数和提供的数组设置数据指针和行宽(linesizes). avpicture_fill函数将ptr指向的数据填充到picture内，但并没有拷贝，只是将picture结构内的data指针指向了ptr的数据!
    avpicture_fill((AVPicture*) pFrameRGB, out_buffer_rgb, PIX_FMT_BGR32, pCodecCtx->width, pCodecCtx->height);

    for(;;)
    {
        if(is->quit)
        {
            qDebug()<<__FUNCTION__<<"quit!";
            packet_queue_flush(&is->videoq);
            break;
        }
        if(is->isPause == true)
        {
            SDL_Delay(10);
            continue;
        }
        if(packet_queue_get(&is->videoq,packet,0) < 0)
        {
            if(is->readFinished)
            {
                break;//队列里没有数据了
            }
            else{
                SDL_Delay(1);//队列里暂时没有数据
                continue;
            }
        }
        //收到这个数据 说明刚刚执行过跳转 现在需要把解码器的数据 清除一下
        if(strcmp((char*)packet->data, FLUSH_DATA) == 0)
        {
            avcodec_flush_buffers(is->video_st->codec);
            av_free_packet(packet);
            continue;
        }
        //解码一帧
        ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
        if(ret<0)
        {
            qDebug()<<"decode error. \n";
            av_free_packet(packet);
            continue;
        }
        //部分情况下ffmpeg 解码出的帧会没有数据 pts值为AV_NOPTS_VALUE=-9223372036854775808
        if(packet->dts == AV_NOPTS_VALUE &&pFrame->opaque && *(uint64_t*)pFrame->opaque !=AV_NOPTS_VALUE)
        {//pFrame->opaque for some private data of the user
            video_pts = *(uint64_t*)pFrame->opaque;
        }
        else if(packet->dts != AV_NOPTS_VALUE)
        {
            video_pts = packet->dts;
        }
        else
        {
            video_pts = 0;
        }
        //做音视频同步
        video_pts *=av_q2d(is->video_st->time_base);
        video_pts = synchronize_video(is,pFrame,video_pts);
        //转跳seek了
        if(is->seek_flag_video)
        {
            //发生转跳，则跳过关键帧到目的时间这几帧
            if(video_pts < is->seek_time)
            {
                av_free_packet(packet);
                continue;
            }
            else{
                is->seek_flag_video = 0;
            }
        }
        while(1)
        {
            if(is->quit)
            {
                break;
            }
            if(is->readFinished && is->audioq.size == 0)
            {
                //读取完了 且音频数据也播放完了 就剩下视频数据了  直接显示出来了 不用同步了
                break;
            }
            audio_pts = is->audio_clock;

            video_pts = is->video_clock;

           if(video_pts <= audio_pts) break;
           int delayTime = (video_pts - audio_pts) * 1000;
           delayTime = delayTime > 5? 5 :delayTime;
           if(!is->isNeedPause)
           {
               SDL_Delay(delayTime);
           }
           //发送展示信号
           if(got_picture)
           {
               //转换
               sws_scale(img_convert_ctx,
                         (uint8_t const *const *)pFrame->data,
                         pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data,
                         pFrameRGB->linesize);
               QImage tmpImg((uchar*)out_buffer_rgb, pCodecCtx->width,pCodecCtx->height, QImage::Format_RGB32);
               //好像是字节对齐的函数，第二个参数是设定有没有Alpha通道吧,4*8=32位，我们只用到24位
               QImage image = tmpImg.convertToFormat(QImage::Format_RGB888,Qt::NoAlpha);

               is->player->disPlayVideo(image);//调用激发信号的函数
               if(is->isNeedPause)
               {
                   is->isPause = true;
                   is->isNeedPause = false;
               }
           }
           av_free_packet(packet);
        }
        av_free(pFrame);
        av_free(pFrameRGB);
        av_free(out_buffer_rgb);
        sws_freeContext(img_convert_ctx);
        if(is->quit){
            is->quit = true;
        }
        is->videoThreadFinished = true;
        qDebug()<<__FUNCTION__<<"finished!";
        return 0;
    }
}
//构造函数
kunplay_thread::kunplay_thread()
{
    //安全起见
    memset(&mVideoState, 0, sizeof(VideoState));//安全起见，初始化内存。
    mVideoWidget = NULL;
    mPlayState = Stop;

    mAudioID = 0;
    mIsMute = 0;
    mVolume = 1;

}

kunplay_thread::~kunplay_thread()
{
    qDebug()<<__FUNCTION__<<"1111.....";
    //貌似这个mAudioID是执行SDL播放的时候得到的设备ID？
    if(mAudioID != 0)
    {
        SDL_LockAudioDevice(mAudioID);
        SDL_CloseAudioDevice(mAudioID);
        SDL_UnlockAudioDevice(mAudioID);
        mAudioID = 0;
    }
    //这个是反初始化，不与AVPacket队列有关
    deInit();

    qDebug()<<__FUNCTION__<<"222...";
}

void kunplay_thread::deInit()
{
    if(mVideoState.swr_ctx != NULL)
    {
        swr_free(&mVideoState.swr_ctx);
        mVideoState.swr_ctx = NULL;
    }

    if(mVideoState.audio_frame != NULL)
    {
        avcodec_free_frame(&mVideoState.audio_frame);
        mVideoState.audio_frame = NULL;
    }
}
//设置文件名就启动线程
bool kunplay_thread::setFileName(QString path)
{
    if(mPlayState != Stop)
    {
        return false;
    }

    memset(&mVideoState , 0, sizeof(VideoState));//安全
    this->start();//启动线程
}

bool kunplay_thread::replay()
{
    while(this->isRunning())
    {
        SDL_Delay(5);
    }
    if(mPlayState != Stop)
        return false;

    this->start();
    return true;
}

bool kunplay_thread::paly()
{
    mVideoState.isNeedPause = false;
    mVideoState.isPause = false;

    if(mPlayState != Pause)
    {
        return false;
    }
    mPlayState = playing;
    emit sig_StateChanged(playing);

    return true;
}

bool kunplay_thread::stop(bool isWait)
{
    qDebug()<<__FUNCTION__<"111.....";
    if(mPlayState == Stop)
    {
        qDebug()<<__FUNCTION__<<"3333";
        return false;
    }
    mPlayState = Stop;
        mVideoState.quit = true;
    qDebug()<<__FUNCTION__<<"222...";
        if (isWait)
        {
            while(!mVideoState.readThreadFinished)
            {
    //            qDebug()<<mVideoState.readThreadFinished<<mVideoState.videoThreadFinished;
                SDL_Delay(3);
            }
        }
    qDebug()<<__FUNCTION__<<"999...";

    //    emit sig_StateChanged(Stop);

        return true;
}

void kunplay_thread::seek(int64_t pos)
{
    if(!mVideoState.seek_req) //seek_req为0进来，应该哪里会置零的，应该是转跳完置零
    {
        mVideoState.seek_pos = pos;
        mVideoState.seek_req = 1;
    }
}

void kunplay_thread::setVolume(float value)
{
    mVolume = value;
    mVideoState.mVolume = value;
}

double kunplay_thread::getCurrentTime()
{
    return mVideoState.audio_clock;
}
//这应该是获取视频时长
int64_t kunplay_thread::getTotalTime()
{
    return mVideoState.ic->duration;
}
void kunplay_thread::disPlayVideo(QImage img)
{
    emit sig_GetOneFrame(img);  //发送信号
}

void kunplay_thread::setVideoWidget(kunplay_showvideowight *widget)
{
    mVideoWidget = widget;
    //关联信号和槽还能写在这
    connect(this,SIGNAL(sig_GetOneFrame(QImage)),mVideoWidget,SLOT(slotGetOneFrame(QImage)));
}

int kunplay_thread :: openSDL()
{
    VideoState *is = &mVideoState;
    //spec 规格
    SDL_AudioSpec wanted_spec, spec;
    int64_t wanted_channel_layout = 0;
    int wanted_nb_channels = 2;
    int samplerate = 44100;

    /*  SDL支持的声道数为 1, 2, 4, 6 */
   //    /*  后面我们会使用这个数组来纠正不支持的声道数目 */
   //    const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
    if(!wanted_channel_layout ||
            wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_nb_channels))
    {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_spec.channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.freq = samplerate;
    if(wanted_spec.freq <= 0 || wanted_spec.channels <= 0){
        fprintf(stderr,"Invalid sample rate or channel count!\n");
        return -1;
    }
    wanted_spec.format = AUDIO_S16SYS; // 具体含义请查看“SDL宏定义”部分
    wanted_spec.silence = 0;             // 0指示静音
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE; // 自定义SDL缓冲区大小
    wanted_spec.callback = audio_callback; // 音频解码的关键回调函数
    wanted_spec.userdata = is; // 传给上面回调函数的外带数据
    //这里是获取音频设备的所有下标，0应该是默认输出设备
    int num = SDL_GetNumAudioDevices(0);
    for(int i = 0 ; i<num ; i++){
        //该变量是类的成员变量, 根据wanted_space规格去找到设备id,space应该也是出参，同为SDL_AudioSpec,space是实际得到的参数
        mAudioID = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(1,0),false, &wanted_spec,&spec,0);
        if(mAudioID>0)
        {//貌似0不是设备的下标，算是一个默认值
            break;
        }
    }
    /* 检查实际使用的配置（保存在spec,由SDL_OpenAudio()填充） */
    if(spec.format != AUDIO_S16SYS)
    {
        qDebug()<<"SDL advised audio format %d is not supported!"<<spec.format;
        return -1;
    }
    if(spec.channels != wanted_nb_channels){
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if(!wanted_channel_layout){
            fprintf(stderr,"SDL advised channel count %d is not supported!\n",spec.channels);
            return -1;
        }
    }
    is->audio_hw_buf_size = spec.size;
    /* 把设置好的参数保存到大结构中 */
    is->audio_src_fmt = is->audio_tgt_fmt = AV_SAMPLE_FMT_S16;
    is->audio_src_freq = is->audio_tgt_freq = spec.freq;
    is->audio_src_channel_layout = is->audio_tgt_channel_layout = wanted_channel_layout;
    is->audio_src_channels = is->audio_tgt_channels = spec.channels;

    //算是给大结构体里的参数初始化吧
    is->audio_buf_size = 0;
    is->audio_buf_index = 0;
    memset(&is->audio_pkt, 0 , sizeof(is->audio_pkt));
    return 0;
}

void kunplay_thread::closeSDL()
{
    if (mAudioID > 0)
    {
        SDL_CloseAudioDevice(mAudioID);
    }

    mAudioID = -1;
}


void kunplay_thread::run()
{
    char file_path[1280] = {0};
    strcpy(file_path, mFileName.toUtf8().data());
    //初始化大结构体，成员变量不是指针，已经分配内存空间
    memset(&mVideoState,0 ,sizeof(VideoState));

    mVideoState.isMute = mIsMute;
    mVideoState.mVolume = mVolume;

    VideoState *is = &mVideoState;

    AVFormatContext *pFormatCtx;
    AVCodecContext *pCodecCtx;
    AVCodec *pCodec;

    AVCodecContext *aCodecCtx;
    AVCodec *aCodec;
    int audioStream, videoStream, i;

    //Allocate an AVFormatContext.
    pFormatCtx = avformat_alloc_context();
    //打开视频
    if(avformat_open_input(&pFormatCtx, file_path, NULL, NULL)!=0){
        printf("can't open file.\n");
        return ;
    }
    if(avformat_find_stream_info(pFormatCtx,NULL)<0){
        printf("could't find stream infomation.\n");
        return ;
    }
    //成员变量的音视频下标
    videoStream = -1;
    audioStream = -1;

    for(i = 0; i<pFormatCtx->nb_streams ; i++){
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO){
            videoStream = i;
        }
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO){
            audioStream = i;
        }
    }

    ///如果videoStream为-1 说明没有找到视频流
    if (videoStream == -1) {
        printf("Didn't find a video stream.\n");
        return;
    }

    if (audioStream == -1) {
        printf("Didn't find a audio stream.\n");
        return;
    }
    //赋值给大结构体
    is->ic = pFormatCtx;
    is->videoStream = videoStream;
    is->audioStream = audioStream;

    emit sig_TotalTimeChanged(getTotalTime());

    if(audioStream >= 0){
        /* 所有设置SDL音频流信息的步骤都在这个函数里完成 */
        audio_stream_component_open(&mVideoState, audioStream);
    }
    ///查找音频解码器
    aCodecCtx = pFormatCtx->streams[audioStream]->codec;
    aCodec = avcodec_find_decoder(aCodecCtx->codec_id);

    if (aCodec == NULL) {
        printf("ACodec not found.\n");
        return;
    }

    ///打开音频解码器
    if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0) {
        printf("Could not open audio codec.\n");
        return;
    }

    is->audio_st = pFormatCtx->streams[audioStream];

    ///查找视频解码器
    pCodecCtx = pFormatCtx->streams[videoStream]->codec;
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);

    if (pCodec == NULL) {
        printf("PCodec not found.\n");
        return;
    }

    ///打开视频解码器
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        printf("Could not open video codec.\n");
        return;
    }
    //这里保存了视频流,初始化了这个指针，视频解码和音画同步都会用到这个参数
    is->video_st = pFormatCtx->streams[videoStream];

    //这个队列结构体不是指针，里面具体的变量由ffmpeg的函数赋内存
    packet_queue_init(&mVideoState.videoq);
    packet_queue_init(&mVideoState.audioq);

    ///创建一个线程专门用来解码视频
    is->video_tid = SDL_CreateThread(video_thread, "video_thread", &mVideoState);

    //这个指针不知道是用来干嘛的，前面用来调用this的信号函数
    is->player = this;

    AVPacket *packet = (AVPacket *) malloc(sizeof(AVPacket));

    av_dump_format(pFormatCtx, 0, file_path, 0); //输出视频信息

    qDebug()<<__FUNCTION__<<is->quit;
    mPlayState = playing;

    emit sig_StateChanged(playing);
    //初始化SDL的参数
    openSDL();

    SDL_LockAudioDevice(mAudioID);
    SDL_PauseAudioDevice(mAudioID,0);
    SDL_UnlockAudioDevice(mAudioID);

    while(1)
    {
        if (is->quit)
        {
            //停止播放了
            break;
        }
        //转跳
        if(is->seek_req)
        {
            int stream_index = -1;
            int64_t seek_target = is->seek_pos;

            if(is->videoStream >= 0)
                stream_index = is->videoStream;
            else if(is->audioStream >= 0)
                stream_index = is->audioStream;
            AVRational aVRational = {1,AV_TIME_BASE};
            if(stream_index >= 0)
            {
                //time_base转换函数av_rescale_q
                seek_target = av_rescale_q(seek_target,aVRational,pFormatCtx->streams[stream_index]->time_base);
            }

            if(av_seek_frame(is->ic,stream_index, seek_target,AVSEEK_FLAG_BACKWARD)<0){
                fprintf(stderr, "%s: error while seeking\n",is->ic->filename);
            }else{
                if(is->audioStream >= 0){
                    AVPacket *packet = (AVPacket *)malloc(sizeof (AVPacket));
                    av_new_packet(packet, 10);
                    strcpy((char*)packet->data,FLUSH_DATA);
                    packet_queue_flush(&is->audioq); //清除队列
                    packet_queue_put(&is->audioq, packet); //往队列中存入用来清除的包
                }
                if(is->videoStream >= 0){
                    AVPacket *packet = (AVPacket *) malloc(sizeof(AVPacket)); //分配一个packet
                    av_new_packet(packet, 10);
                    strcpy((char*)packet->data,FLUSH_DATA);
                    packet_queue_flush(&is->videoq); //清除队列
                    packet_queue_put(&is->videoq, packet); //往队列中存入用来清除的包
                    is->video_clock = 0;
                }
            }
            //置零
            is->seek_req = 0;
            is->seek_time = is->seek_pos / 1000000.0;//是微妙转换成秒吗?
            is->seek_flag_audio = 1;//这两个标志位忘记在哪用的了
            is->seek_flag_video = 1;

            if (is->isPause)
            {
                is->isNeedPause = true;
                is->isPause = false;
            }
        }

        //这里做了个限制  当队列里面的数据超过某个大小的时候 就暂停读取  防止一下子就把视频读完了，导致的空间分配不足
        /* 这里audioq.size是指队列中的所有数据包带的音频数据的总量或者视频数据总量，并不是包的数量 */
        //这个值可以稍微写大一些
        //        qDebug()<<__FUNCTION__<<is->audioq.size<<MAX_AUDIO_SIZE<<is->videoq.size<<MAX_VIDEO_SIZE;
        if (is->audioq.size > MAX_AUDIO_SIZE || is->videoq.size > MAX_VIDEO_SIZE) {//队列里缓存的最大数据大小
            SDL_Delay(10);
            continue;
        }
        //        qDebug()<<__FUNCTION__<<"is->isPause"<<is->isPause;

        if (is->isPause == true)
        {
            SDL_Delay(10);
            continue;
        }

        if (av_read_frame(pFormatCtx, packet) < 0)//这里是读文件流成一帧
        {
            is->readFinished = true;

//            qDebug()<<__FUNCTION__<<"av_read_frame<0";

            if (is->quit)
            {
                break; //解码线程也执行完了 可以退出了
            }

            SDL_Delay(10);
            continue;
        }
        if (packet->stream_index == videoStream)
        {
            packet_queue_put(&is->videoq, packet);
            //这里我们将数据存入队列 因此不调用 av_free_packet 释放
        }
        else if( packet->stream_index == audioStream )
        {
            packet_queue_put(&is->audioq, packet);
            //这里我们将数据存入队列 因此不调用 av_free_packet 释放
        }
        else
        {
            // Free the packet that was allocated by av_read_frame
            av_free_packet(packet);
        }
    }
    qDebug()<<__FUNCTION__<<"333";

    ///文件读取结束 跳出循环的情况
    ///等待播放完毕
    while (!is->quit) {
        SDL_Delay(100);
    }
    qDebug()<<__FUNCTION__<<"555";

    avcodec_close(aCodecCtx);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
    avformat_free_context(pFormatCtx);

    free(packet);

    packet_queue_deinit(&mVideoState.videoq);
    packet_queue_deinit(&mVideoState.audioq);

    is->readThreadFinished = true;

    emit sig_StateChanged(Stop);

    qDebug()<<__FUNCTION__<<"finished!";

    return ;
}




































