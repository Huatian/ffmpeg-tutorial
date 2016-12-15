#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mathematics.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#include <stdio.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define FIX_INPUT 1

typedef struct PacketQueue
{
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;

int quit = 0;

AVFrame wanted_frame;

void packet_queue_init (PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{

    AVPacketList *pkt1;
    if(av_dup_packet(pkt) < 0)
    {
        return -1;
    }

    pkt1 = av_malloc(sizeof(AVPacketList));
    if(!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if(!q->last_pkt)
    {
        q->first_pkt = pkt1;
    }
    else
    {
        q->last_pkt->next = pkt1;
    }

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);

    return 0;
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    for(;;)
    {

        if(quit)
        {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if(pkt1)
        {
            q->first_pkt = pkt1->next;
            if(!q->first_pkt)
                q->last_pkt = NULL;

            q->nb_packets--;
            q->size -= pkt1->pkt.size;

            *pkt = pkt1->pkt;

            av_free(pkt1);
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

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size)
{

    static AVPacket pkt;
    static int audio_pkt_size = 0;
    static AVFrame frame;

    int len1, data_size = 0;

    SwrContext *swr_ctx = NULL;

    int resampled_data_size;

    for(;;)
    {
        if(pkt.data)
            av_free_packet(&pkt);


        if(quit)
        {
            return -1;
        }

        if(packet_queue_get(&audioq, &pkt, 1) < 0)
        {
            return -1;
        }

        audio_pkt_size = pkt.size;

        while(audio_pkt_size > 0)
        {

            int got_frame = 0;
            len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);

           // printf( "pkt size=%d \n", pkt.size);
            //printf( "frame size=%u \n", frame.data);
            //printf( "codec size=%d \n",len1);
            //printf("got_frame=%d \n", got_frame);

            if(len1 < 0)
            {
                audio_pkt_size = 0;
                break;
            }

            audio_pkt_size -= len1;

            if(!got_frame)
                continue;

            data_size = av_samples_get_buffer_size(NULL, aCodecCtx->channels, frame.nb_samples,
                                                   AUDIO_S16SYS, 1);

            if (frame.channels > 0 && frame.channel_layout == 0)
                frame.channel_layout = av_get_default_channel_layout(frame.channels);
            else if (frame.channels == 0 && frame.channel_layout > 0)
                frame.channels = av_get_channel_layout_nb_channels(frame.channel_layout);

            printf("frame.sample_rate = %d \n", frame.sample_rate);
            printf("frame.format = %d \n", frame.format);
            printf("frame.format bits = %d \n", av_get_bytes_per_sample(frame.format));
            printf("frame.channels = %d \n", frame.channels);
            printf("frame.channel_layout = %d \n", frame.channel_layout);
            printf("frame.nb_samples = %d \n", frame.nb_samples);
            printf("\n");

            /**
             * 接下来判断我们之前设置SDL时设置的声音格式(AV_SAMPLE_FMT_S16)，声道布局，
             * 采样频率，每个AVFrame的每个声道采样数与
             * 得到的该AVFrame分别是否相同，如有任意不同，我们就需要swr_convert该AvFrame，
             * 然后才能符合之前设置好的SDL的需要，才能播放
             */
            if(frame.format != AUDIO_S16SYS
                    || frame.channel_layout != aCodecCtx->channel_layout
                    || frame.sample_rate != aCodecCtx->sample_rate
                    || frame.nb_samples != SDL_AUDIO_BUFFER_SIZE)
            {

                if (swr_ctx != NULL)
                {
                    swr_free(&swr_ctx);
                    swr_ctx = NULL;
                }

                swr_ctx = swr_alloc_set_opts(NULL, wanted_frame.channel_layout, (enum AVSampleFormat)wanted_frame.format, wanted_frame.sample_rate,
                                             frame.channel_layout, (enum AVSampleFormat)frame.format, frame.sample_rate, 0, NULL);

                if (swr_ctx == NULL || swr_init(swr_ctx) < 0)
                {
                    fprintf(stderr, "swr_init failed: \n" );
                    break;
                }
            }

            if(swr_ctx)
            {
                int dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, frame.sample_rate) + frame.nb_samples,
                                                    wanted_frame.sample_rate, wanted_frame.format, AV_ROUND_INF);
                printf("swr convert ! \n");
                printf("dst_nb_samples : %d \n", dst_nb_samples);
                /**
                  * 转换该AVFrame到设置好的SDL需要的样子，有些旧的代码示例最主要就是少了这一部分，
                  * 往往一些音频能播，一些不能播，这就是原因，比如有些源文件音频恰巧是AV_SAMPLE_FMT_S16的。
                  * swr_convert 返回的是转换后每个声道(channel)的采样数
                  */
                int len2 = swr_convert(swr_ctx, &audio_buf, dst_nb_samples,(const uint8_t**)frame.data, frame.nb_samples);
                if (len2 < 0)
                {
                    fprintf(stderr, "swr_convert failed \n" );
                    break;
                }

                resampled_data_size = wanted_frame.channels * len2 * av_get_bytes_per_sample((enum AVSampleFormat)wanted_frame.format);
            }else{
                resampled_data_size = data_size;
            }



            return resampled_data_size;
        }

    }
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{

    AVCodecContext *aCodecCtx = (AVCodecContext *) userdata;
    int len1, audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    SDL_memset(stream, 0, len);

    //printf("audio_callback len=%d \n", len);

    //向设备发送长度为len的数据
    while(len > 0)
    {
        //缓冲区中无数据
        if(audio_buf_index >= audio_buf_size)
        {
            //从packet中解码数据
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, audio_buf_size);
            //printf("audio_decode_frame finish  audio_size=%d \n", audio_size);
            if(audio_size < 0) //没有解码到数据或者出错，填充0
            {
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
            }
            else
            {
                audio_buf_size = audio_size;
            }

            audio_buf_index = 0;
        }

        len1 = audio_buf_size - audio_buf_index;
        if(len1 > len)
            len1 = len;

        //memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        SDL_MixAudio(stream, audio_buf + audio_buf_index, len1, SDL_MIX_MAXVOLUME);

        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}


int main(int argc, char *argv[])
{
    AVFormatContext *pFormatCtx = NULL;
    int             i, audioStream;

    AVPacket         packet;

    AVCodecContext  *aCodecCtx = NULL;
    AVCodec         *aCodec = NULL;

    SDL_AudioSpec   wanted_spec, spec;

    SDL_Event       event;

    char filename[100];

#ifdef FIX_INPUT
    strcpy(filename, "/home/wanghuatian/oceans.mp4");
#else
    if(argc < 2)
    {
        fprintf(stderr, "Usage: test <file> \n");
        exit(1);
    }

    strcpy(filename, argv[1]);
#endif // FIX_INPUT



    av_register_all();

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        fprintf(stderr,"Could not initialize SDL - %s " + *SDL_GetError());
        exit(1);
    }

    // 读取文件头，将格式相关信息存放在AVFormatContext结构体中
    if(avformat_open_input(&pFormatCtx, filename, NULL, NULL) != 0)
        return -1;
    // 检测文件的流信息
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1;

    // 在控制台输出文件信息
    av_dump_format(pFormatCtx, 0, filename, 0);

    for(i = 0; i < pFormatCtx->nb_streams; i++)
    {
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
            audioStream = i;
    }

    aCodecCtx = pFormatCtx->streams[audioStream]->codec;

    aCodec = avcodec_find_decoder(aCodecCtx->codec_id);
    if(!aCodec)
    {
        fprintf(stderr, "Unsupported codec ! \n");
        return -1;
    }

    wanted_spec.freq = aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;

    printf("codecCtx sample_rate = %d \n", aCodecCtx->sample_rate);
    printf("codecCtx channels = %d \n", aCodecCtx->channels);
    printf("codecCtx sample_fmt = %d \n", aCodecCtx->sample_fmt);
    printf("AUDIO_S16SYS = %d \n", AUDIO_S16SYS);
    printf("\n");

    /**
     *SDL_OpenAudio 函数通过wanted_spec来打开音频设备，成功返回零，将实际的硬件参数传递给spec的指向的结构体。
     *如果spec为NULL，audio data将通过callback函数，保证将自动转换成硬件音频格式。
     *
     *音频设备刚开始播放静音，当callback变得可用时，通过调用SDL_PauseAudio(0)来开始播放。
     *由于audio diver 可能修改音频缓存的请求大小，所以你应该申请任何的混合缓存（mixing buffers），在你打开音频设备之后。*/
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0)
    {
        fprintf(stderr, "SDL_OpenAudio: %s \n", SDL_GetError());
        return -1;
    }

    printf("spec freq = %d \n", spec.freq);
    printf("spec format = %d \n", spec.format);
    printf("spec channels = %d \n", spec.channels);
    printf("spec samples = %d \n", spec.samples);
    printf("spec silence = %d \n", spec.silence);
    printf("spec padding = %d \n", spec.padding);
    printf("spec size = %d \n", spec.size);
    printf("\n");

    printf("AV_SAMPLE_FMT_S16 = %d \n", AV_SAMPLE_FMT_S16);
    wanted_frame.format = AV_SAMPLE_FMT_S16;
    wanted_frame.sample_rate = spec.freq;
    wanted_frame.channel_layout = av_get_default_channel_layout(spec.channels);
    wanted_frame.channels = spec.channels;

    avcodec_open2(aCodecCtx, aCodec, NULL);

    packet_queue_init(&audioq);
    SDL_PauseAudio(0);


    while (av_read_frame(pFormatCtx, &packet) >= 0)
    {
        if (packet.stream_index == audioStream)
            packet_queue_put(&audioq, &packet);
        else
            av_free_packet(&packet);

        SDL_PollEvent(&event);

        switch (event.type) {

            case SDL_QUIT:
                quit = 1;
                SDL_Quit();
                exit(0);
                break;

            default:
                break;
        }
    }

    getchar();
   // avformat_close_input(&pFormatCtx);

    return 0;
}
