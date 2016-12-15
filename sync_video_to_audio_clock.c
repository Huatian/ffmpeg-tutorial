#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mathematics.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#include <stdio.h>
#include <math.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define SDL_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 *1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

#define VIDEO_PICTURE_QUEUE_SIZE 1

#define FIX_INPUT 0

typedef struct PacketQueue
{
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct VideoPicture
{
    SDL_Overlay *bmp;
    int width, height;
    int allocated;
    double pts;
} VideoPicture;

typedef struct VideoState
{

    AVFormatContext *pFormatCtx;
    int             videoStream, audioStream;

    AVStream        *audio_st;
    PacketQueue     audioq;
    uint8_t         audio_buf[(192000 * 3) / 2];
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    AVFrame         audio_frame, wanted_frame;
    AVPacket        audio_pkt;
    uint8_t         *audio_pkt_data;
    int             audio_pkt_size;
    /* 时钟同步变量 */
    double          audio_clock;
    int             audio_hw_buf_size;
    double          frame_timer;
    double          frame_last_pts;
    double          frame_last_delay;
    double          video_clock;

    AVStream        *video_st;
    PacketQueue     videoq;

    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;

    SDL_Thread      *parse_tid;
    SDL_Thread      *video_tid;

    char            filename[1024];
    int             quit;

    AVIOContext     *io_context;
    struct SwsContext      *sws_ctx;

} VideoState;

SDL_Surface     *screen;
VideoState      *global_video_state;

void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{

    AVPacketList *pkt_list;
    if(av_dup_packet(pkt) < 0)
    {
        return -1;
    }

    pkt_list = av_malloc(sizeof(AVPacketList));
    if(!pkt_list)
        return -1;
    pkt_list->pkt = *pkt;
    pkt_list->next = NULL;

    SDL_LockMutex(q->mutex);

    if(!q->last_pkt)
        q->first_pkt = pkt_list;
    else
        q->last_pkt->next = pkt_list;

    q->last_pkt = pkt_list;
    q->nb_packets++;
    q->size += pkt->size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt_list;
    int ret;

    SDL_LockMutex(q->mutex);

    for(;;)
    {

        if(global_video_state->quit)
        {
            ret = -1;
            break;
        }

        pkt_list = q->first_pkt;
        if(pkt_list)
        {
            q->first_pkt = pkt_list->next;
            if(!q->first_pkt)
                q->last_pkt = NULL;

            q->nb_packets--;
            q->size -= pkt_list->pkt.size;

            *pkt = pkt_list->pkt;
            av_free(pkt_list);
            ret = 1;
            break;
        }
        else if(!block)
        {
            ret = 0;
            break;
        }
        else
        {
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);
    return ret;
}

double get_audio_clock(VideoState *is){
    double pts;
    int hw_buf_size, bytes_per_sec, n;

    pts = is->audio_clock;
    hw_buf_size = is->audio_buf_size - is->audio_buf_index;
    bytes_per_sec = 0;
    n = is->audio_st->codec->channels * 2;

    if(is->audio_st){
        bytes_per_sec = is->audio_st->codec->sample_rate * n;
    }

    if(bytes_per_sec){
        pts -= (double)hw_buf_size / bytes_per_sec;
    }

    return pts;
}

int audio_decode_frame(VideoState *is)
{

    AVPacket *pkt = &is->audio_pkt;

    int len1, data_size = 0;

    SwrContext *swr_ctx = NULL;

    int resampled_data_size;

    uint8_t *buf = &is->audio_buf;
    AVFrame *frame = &is->audio_frame;

    for(;;)
    {
        if(pkt->data)
            av_free_packet(pkt);


        if(is->quit)
        {
            return -1;
        }

        if(packet_queue_get(&is->audioq, pkt, 1) < 0)
        {
            return -1;
        }

        is->audio_pkt_size = pkt->size;

        /* if update, update the audio clock w/pts */
        if(pkt->pts != AV_NOPTS_VALUE) {
          is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
        }

        while(is->audio_pkt_size > 0)
        {

            int got_frame = 0;
            len1 = avcodec_decode_audio4(is->audio_st->codec, &is->audio_frame, &got_frame, pkt);

            if(len1 < 0)
            {
                is->audio_pkt_size = 0;
                break;
            }

            is->audio_pkt_size -= len1;

            if(!got_frame)
                continue;


            if (is->audio_frame.channels > 0 && is->audio_frame.channel_layout == 0)
                is->audio_frame.channel_layout = av_get_default_channel_layout(is->audio_frame.channels);
            else if (is->audio_frame.channels == 0 && is->audio_frame.channel_layout > 0)
                is->audio_frame.channels = av_get_channel_layout_nb_channels(is->audio_frame.channel_layout);

            /**
             * 接下来判断我们之前设置SDL时设置的声音格式(AV_SAMPLE_FMT_S16)，声道布局，
             * 采样频率，每个AVFrame的每个声道采样数与
             * 得到的该AVFrame分别是否相同，如有任意不同，我们就需要swr_convert该AvFrame，
             * 然后才能符合之前设置好的SDL的需要，才能播放
             */
            if(is->audio_frame.format != is->wanted_frame.format
                    || is->audio_frame.channel_layout != is->wanted_frame.channel_layout
                    || is->audio_frame.sample_rate != is->wanted_frame.sample_rate
                    || is->audio_frame.nb_samples != SDL_AUDIO_BUFFER_SIZE)
            {

                if (swr_ctx != NULL)
                {
                    swr_free(&swr_ctx);
                    swr_ctx = NULL;
                }

                swr_ctx = swr_alloc_set_opts(NULL, is->wanted_frame.channel_layout, (enum AVSampleFormat)is->wanted_frame.format, is->wanted_frame.sample_rate,
                                             is->audio_frame.channel_layout, (enum AVSampleFormat)is->audio_frame.format, is->audio_frame.sample_rate, 0, NULL);

                if (swr_ctx == NULL || swr_init(swr_ctx) < 0)
                {
                    fprintf(stderr, "swr_init failed: \n" );
                    break;
                }
            }

            if(swr_ctx)
            {
                int dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, is->audio_frame.sample_rate) + is->audio_frame.nb_samples,
                                                    is->wanted_frame.sample_rate, is->wanted_frame.format, AV_ROUND_INF);
                printf("swr convert !%d \n", dst_nb_samples);
                printf("audio_frame.nb_samples : %d \n", is->audio_frame.nb_samples);
                printf("is->audio_buf : %d \n", *is->audio_buf);
                printf("is->audio_buf : %d \n", &is->audio_buf);
                printf("is->buf : %d \n", &buf);
                printf("is->buf : %d \n", *buf);
                /**
                  * 转换该AVFrame到设置好的SDL需要的样子，有些旧的代码示例最主要就是少了这一部分，
                  * 往往一些音频能播，一些不能播，这就是原因，比如有些源文件音频恰巧是AV_SAMPLE_FMT_S16的。
                  * swr_convert 返回的是转换后每个声道(channel)的采样数
                  */
                int len2 = swr_convert(swr_ctx, &buf, dst_nb_samples,(const uint8_t**)&is->audio_frame.data, is->audio_frame.nb_samples);
                if (len2 < 0)
                {
                    fprintf(stderr, "swr_convert failed \n" );
                    break;
                }

                resampled_data_size = is->wanted_frame.channels * len2 * av_get_bytes_per_sample((enum AVSampleFormat)is->wanted_frame.format);
            }
            else
            {
                data_size = av_samples_get_buffer_size(NULL, is->audio_st->codec->channels, is->audio_frame.nb_samples,
                                                   AUDIO_S16SYS, 1);
                resampled_data_size = data_size;
            }

            is->audio_clock += (double)resampled_data_size /
                (double)(2 * is->audio_st->codec->channels * is->audio_st->codec->sample_rate);

            return resampled_data_size;
        }

    }
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{

    VideoState *is = (VideoState *) userdata;
    int len1, audio_size;

    SDL_memset(stream, 0, len);

    printf("audio_callback len=%d \n", len);

    //向设备发送长度为len的数据
    while(len > 0)
    {
        //缓冲区中无数据
        if(is->audio_buf_index >= is->audio_buf_size)
        {
            //从packet中解码数据
            audio_size = audio_decode_frame(is);
            printf("audio_decode_frame finish  audio_size=%d \n", audio_size);
            if(audio_size < 0) //没有解码到数据或者出错，填充0
            {
                is->audio_buf_size = 1024;
                memset(is->audio_buf, 0, is->audio_buf_size);
            }
            else
            {
                is->audio_buf_size = audio_size;
            }

            is->audio_buf_index = 0;
        }

        len1 = is->audio_buf_size - is->audio_buf_index;
        if(len1 > len)
            len1 = len;

        //memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        SDL_MixAudio(stream, is->audio_buf + is->audio_buf_index, len1, SDL_MIX_MAXVOLUME);

        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque)
{
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;
}

//让视频按固定延迟时间刷新
static void schedule_refresh(VideoState *is, int delay)
{
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is)
{

    SDL_Rect rect;
    VideoPicture *vp;

    float aspect_ratio;//宽高比例
    int w, h, x, y;

    vp = &is->pictq[is->pictq_rindex];
    if(vp->bmp)
    {
        if(is->video_st->codec->sample_aspect_ratio.num == 0)
        {
            aspect_ratio = 0;
        }
        else
        {
            aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio) *
                           is->video_st->codec->width / is->video_st->codec->height;
        }

        if(aspect_ratio <= 0.0)
        {
            aspect_ratio = (float)is->video_st->codec->width / (float)is->video_st->codec->height;
        }

        h = screen->h;
        w = ((int) rint(h * aspect_ratio)) & -3;
        if(w > screen->w)
        {
            w = screen->w;
            h = ((int) rint(w / aspect_ratio)) & -3;
        }

        x = (screen->w - w) / 2;
        y = (screen->h - h) / 2;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        SDL_DisplayYUVOverlay(vp->bmp, &rect);
    }
}


void video_refresh_timer(void *userdata)
{

    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;
    double actual_delay, delay, sync_threshold, ref_clock, diff;

    if(is->video_st)
    {
        if(is->pictq_size == 0)
        {
            schedule_refresh(is, 1);
        }
        else
        {
            vp = &is->pictq[is->pictq_rindex];

            //设置延迟，首先和上次的pts对比得出延迟，更新延迟和pts；
            //通过与音频时钟比较，得到更精确的延迟
            //最后与外部时钟对比，得出最终可用的延迟，并刷新视频
            delay = vp->pts - is->frame_last_pts;
            if(delay <= 0 || delay >= 1.0){
                delay = is->frame_last_delay;//如果延迟不正确，我们使用上一个延迟
            }

            is->frame_last_delay = delay;
            is->frame_last_pts = vp->pts;

            ref_clock = get_audio_clock(is);
            diff = vp->pts - ref_clock;

            sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
            if(fabs(diff) < AV_NOSYNC_THRESHOLD){
                if(diff <= -sync_threshold){//音频快于视频
                    delay = 0;
                }else if(diff >= sync_threshold){//视频快于音频
                    delay = 2 * delay;
                }
            }

            is->frame_timer += delay;

            actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
            if(actual_delay < 0.010){
                actual_delay = 0.010;
            }

            schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));

            video_display(is);

            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)
            {
                is->pictq_rindex = 0;
            }

            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    }
    else
    {
        schedule_refresh(is, 100);
    }
}

void alloc_picture(void *userdata)
{
    VideoState *is = (VideoState *) userdata;
    VideoPicture *vp;

    vp = &is->pictq[is->pictq_windex];
    if(vp->bmp)
        SDL_FreeYUVOverlay(vp->bmp);

    vp->bmp = SDL_CreateYUVOverlay(is->video_st->codec->width,
                                   is->video_st->codec->height,
                                   SDL_YV12_OVERLAY,
                                   screen);

    vp->width = is->video_st->codec->width;
    vp->height = is->video_st->codec->height;

    SDL_LockMutex(is->pictq_mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
}

int queue_picture(VideoState *is, AVFrame *pFrame, double pts)
{

    VideoPicture *vp;
    AVPicture pict;

    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit)
    {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if(is->quit)
        return -1;

    vp = &is->pictq[is->pictq_windex];

    if(!vp->bmp || vp->width != is->video_st->codec->width
            || vp->height != is->video_st->codec->height)
    {
        SDL_Event event;

        vp->allocated = 0;
        //在主线程操作
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);

        //等待直到我们申请到一个picture的内存
        SDL_LockMutex(is->pictq_mutex);
        while(!vp->allocated && !is->quit)
        {
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);
        }
        SDL_UnlockMutex(is->pictq_mutex);

        if(is->quit)
            return -1;
    }

    if(vp->bmp)
    {
        SDL_LockYUVOverlay(vp->bmp);

        pict.data[0] = vp->bmp->pixels[0];
        pict.data[1] = vp->bmp->pixels[2];
        pict.data[2] = vp->bmp->pixels[1];

        pict.linesize[0] = vp->bmp->pitches[0];
        pict.linesize[1] = vp->bmp->pitches[2];
        pict.linesize[2] = vp->bmp->pitches[1];

        sws_scale(is->sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize,
                  0, is->video_st->codec->height, pict.data, pict.linesize);

        SDL_UnlockYUVOverlay(vp->bmp);
        vp->pts = pts;

        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
            is->pictq_windex = 0;

        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

/* 将 PTS 传入大结构体，并计算延迟 */
double synchronize_video(VideoState *is, AVFrame *src_frame, double pts){

    double frame_delay;

    if(pts != 0){
        is->video_clock = pts;
    }else{
        pts = is->video_clock;
    }

    frame_delay = av_q2d(is->video_st->codec->time_base);

    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    is->video_clock += frame_delay;

    return pts;
}

int video_thread(void *arg){
    VideoState *is = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;
    int frameFinished;
    AVFrame *pFrame;
    double pts;

    pFrame = av_frame_alloc();

    for(;;){
        if(packet_queue_get(&is->videoq, packet, 1) < 0)
            break;

        pts = 0;

        avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished, packet);

        if(packet->dts == AV_NOPTS_VALUE && packet->pts && packet->pts != AV_NOPTS_VALUE){
            pts = packet->pts;
        }else if(packet->dts != AV_NOPTS_VALUE){
            pts = packet->dts;
        }else{
            pts = 0;
        }
        pts *= av_q2d(is->video_st->time_base);

        if(frameFinished){
            pts = synchronize_video(is, pFrame, pts);
            if(queue_picture(is, pFrame, pts) < 0)
                break;
        }

        av_free_packet(packet);
    }

    av_free(pFrame);
    return 0;
}

int stream_commponent_open(VideoState *is, int stream_index){
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext  *codecCtx = NULL;
    AVCodec         *codec = NULL;
    AVDictionary    *optionDict = NULL;
    SDL_AudioSpec   wanted_spec, spec;

    if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams)
        return -1;

    codecCtx = pFormatCtx->streams[stream_index]->codec;

    if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO){
        wanted_spec.freq = codecCtx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = codecCtx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;

        if(SDL_OpenAudio(&wanted_spec, &spec)){
            fprintf(stderr, "SDL_OpenAudio: %s \n", SDL_GetError());
            return -1;
        }

        printf("spec format: %d \n", spec.format);
        is->wanted_frame.format = AV_SAMPLE_FMT_S16;
        is->wanted_frame.sample_rate = spec.freq;
        is->wanted_frame.channel_layout = av_get_default_channel_layout(spec.channels);
        is->wanted_frame.channels = spec.channels;
    }

    codec = avcodec_find_decoder(codecCtx->codec_id);
    if(!codec || (avcodec_open2(codecCtx, codec, &optionDict) < 0)){
        fprintf(stderr, "Unsupported codec! \n");
        return -1;
    }

    switch(codecCtx->codec_type){
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream = stream_index;
            is->audio_st = pFormatCtx->streams[stream_index];
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            SDL_PauseAudio(0);
            break;

        case AVMEDIA_TYPE_VIDEO:
            is->videoStream = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];

            is->frame_timer = (double) av_gettime() / 1000000.0;
            is->frame_last_delay = 40e-3;

            packet_queue_init(&is->videoq);
            is->video_tid = SDL_CreateThread(video_thread,is);
            is->sws_ctx = sws_getContext(is->video_st->codec->width,
                                         is->video_st->codec->height,
                                         is->video_st->codec->pix_fmt,
                                         is->video_st->codec->width,
                                         is->video_st->codec->height,
                                         AV_PIX_FMT_YUV420P,
                                         SWS_BILINEAR,NULL,NULL,NULL);

            break;

        default:
            break;
    }
    return 0;
}

int decode_interrupt_cb(void *opaque){
    return (global_video_state && global_video_state->quit);
}

int decode_thread(void *arg){
    VideoState *is = (VideoState *)arg;
    AVFormatContext *pFormatCtx = NULL;
    AVPacket pkt1, *packet = &pkt1;

    int video_index = -1;
    int audio_index = -1;
    int i;

    AVDictionary *io_dict = NULL;
    AVIOInterruptCB callback;

    is->videoStream = -1;
    is->audioStream = -1;

    global_video_state = is;
    callback.callback = decode_interrupt_cb;
    callback.opaque = is;

    if(avio_open2(&is->io_context, is->filename, 0, &callback, &io_dict) != 0){
        fprintf(stderr, "Unable to open I/O for %s \n", is->filename);
        return -1;
    }

    if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0)
        return -1;

    is->pFormatCtx = pFormatCtx;

    if(avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1;

    av_dump_format(pFormatCtx, 0, is->filename, 0);

    for(i=0; i < pFormatCtx->nb_streams; i++){
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0)
            video_index = i;

        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0)
            audio_index = i;
    }

    printf("audio_index:%d, video_index:%d \n", audio_index, video_index);

    if(audio_index >= 0)
        stream_commponent_open(is, audio_index);

    if(video_index >= 0)
        stream_commponent_open(is, video_index);

    if(is->videoStream < 0 || is->audioStream < 0){
        fprintf(stderr, "%s: could not open codecs \n", is->filename);
        goto fail;
    }

    //main decode loop
    for(;;){
        if(is->quit)
            break;

        if(is->audioq.size > MAX_AUDIOQ_SIZE || is->videoq.size > MAX_VIDEOQ_SIZE){
            SDL_Delay(10);
            continue;
        }

        if(av_read_frame(is->pFormatCtx, packet) < 0){
            if(is->pFormatCtx->pb->error == 0){
                SDL_Delay(100);
                continue;
            }else{
                break;
            }
        }

        if(packet->stream_index == is->videoStream){
            packet_queue_put(&is->videoq, packet);
        }else if(packet->stream_index == is->audioStream){
            packet_queue_put(&is->audioq, packet);
        }else{
            av_free_packet(packet);
        }
    }

    while(!is->quit)
        SDL_Delay(100);

    fail:
        if(1){
            SDL_Event event;
            event.type = FF_QUIT_EVENT;
            event.user.data1 = is;
            SDL_PushEvent(&event);
        }

    return 0;
}

int main(int argc, char* argv[]){

#if !FIX_INPUT
    if(argc < 2){
        fprintf(stderr, "Usage: test <file> \n");
        exit(1);
    }
#endif // FIX_INPUT

    SDL_Event event;

    VideoState *is;

    is = (VideoState *)av_mallocz(sizeof(VideoState));

    if (is == NULL)
     {
          fprintf(stderr, "malloc ps error\n");
     }

    av_register_all();

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)){
        fprintf(stderr, "Could not initialize SDL - %s \n", SDL_GetError());
        exit(1);
    }

#ifndef __DARWIN__
        screen = SDL_SetVideoMode(640, 480, 0, 0);
#else
        screen = SDL_SetVideoMode(640, 480, 24, 0);
#endif

    if(!screen){
        fprintf(stderr, "SDL: could not set video mode - exiting \n");
        exit(1);
    }

    #if FIX_INPUT
        strcpy(is->filename, "/home/wanghuatian/oceans.mp4");
    #else
        av_strlcpy(is->filename, argv[1], 1024);
    #endif // FIX_INPUT
    //av_strlcpy(is->filename, argv[1], 1024);
    //char url[] = "/home/wanghuatian/oceans.mp4";
    //av_strlcpy(is->filename, url, 1024);

    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();

    schedule_refresh(is, 40);

    is->parse_tid = SDL_CreateThread(decode_thread, is);
    if(!is->parse_tid){
        av_free(is);
        return -1;
    }

    for(;;){
        SDL_WaitEvent(&event);

        switch (event.type){
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            is->quit = 1;
            SDL_CondSignal(is->audioq.cond);
            SDL_CondSignal(is->videoq.cond);
            return 0;
            break;

        case FF_ALLOC_EVENT:
            alloc_picture(event.user.data1);
            break;

        case FF_REFRESH_EVENT:
            video_refresh_timer(event.user.data1);
            break;

        default:
            break;
        }
    }

    getchar();

    return 0;
}
