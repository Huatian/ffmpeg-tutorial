#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

int main2(int argc, char *argv[]) {
    AVFormatContext *pFormatCtx = NULL;
    int             i, videoStream;
    AVCodecContext  *pCodecCtx = NULL;
    AVCodec         *pCodec = NULL;
    AVFrame         *pFrame = NULL;
    AVPacket        packet;
    int             frameFinished;

    AVDictionary    *optionDict = NULL;
    struct SwsContext *sws_ctx = NULL;

    SDL_Overlay     *bmp = NULL;
    SDL_Surface     *screen = NULL;
    SDL_Rect        rect;
    SDL_Event       event;

    if(argc < 2){
        fprintf(stderr, "Usage: test <file> \n");
        exit(1);
    }

    av_register_all();

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)){
        fprintf(stderr,"Could not initialize SDL - %s " + *SDL_GetError());
        exit(1);
    }

    /**
    *打开一个文件
    */
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
        return -1;

    /**
	 *为pFormatCtx->streams填充上正确的信息
	 */
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1;

    /**
	 *手工调试函数，将文件信息在终端输出
	 */
    av_dump_format(pFormatCtx, 0, argv[1], 0);


    videoStream=-1;
	for ( i = 0; i < pFormatCtx->nb_streams; i++)
	  if(pFormatCtx -> streams[i] -> codec -> codec_type == AVMEDIA_TYPE_VIDEO) {
	    videoStream = i;
	    break;
	  }

	if(videoStream == -1)
	  return -1;

    /**
     *从 vedio stream 中获取对应的解码器上下文的指针
     */
    pCodecCtx = pFormatCtx -> streams[videoStream] -> codec;

    /**
     *根据 codec_id 找到对应的解码器
     */
    pCodec = avcodec_find_decoder(pCodecCtx -> codec_id);

    if(pCodec == NULL){
        fprintf(stderr, "Unsupported codec ! \n");
        return -1;
    }

    /**
     * 打开解码器
     */
    if(avcodec_open2(pCodecCtx, pCodec, &optionDict) <0 )
        return -1;

    /**
     * 为frame 申请内存
     */
    pFrame = av_frame_alloc();

    #ifdef __DARWIN__
        screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 0, 0);
    #else
        screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 24, 0);
    #endif // __DARWIN__

    if(!screen){
        fprintf(stderr, "SDL : could not set video mode - exiting \n");
        exit(1);
    }

    /**
     * 申请一个 overlay , 将 yuv数据给 screen
     */
    bmp = SDL_CreateYUVOverlay(pCodecCtx->width, pCodecCtx->height, SDL_YV12_OVERLAY, screen);

    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
                             AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

    i = 0;
    while (av_read_frame(pFormatCtx, &packet) >= 0){

        if(packet.stream_index == videoStream){
            //为视频流解码
            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

            if(frameFinished){
                SDL_LockYUVOverlay(bmp);

                /**
                 *AVPicture 结构体有一个数据指针指向一个有 4 个元素的指针数组。由于我们处理的是 YUV420P,所以
                 *我们只需要 3 个通道即只要三组数据。其它的格式可能需要第四个指针来表示 alpha 通道或者其它参数。行尺寸
                 *正如它的名字表示的意义一样。在 YUV 覆盖中相同功能的结构体是像素(pixel)和间距(pitch)。(“间距”是
                 *在 SDL 里用来表示指定行数据宽度的值)。所以我们现在做的是让我们的 pict.data 中的三个数组指针指向我们的
                 *覆盖,这样当我们写(数据)到 pict 的时候,实际上是写入到我们的覆盖中,当然要先申请必要的空间。
                 */

                AVPicture pict;
                pict.data[0] = bmp->pixels[0];
                pict.data[1] = bmp->pixels[2];
                pict.data[2] = bmp->pixels[1];

                pict.linesize[0] = bmp->pitches[0];
                pict.linesize[1] = bmp->pitches[2];
                pict.linesize[2] = bmp->pitches[1];

                sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize,
                          0, pCodecCtx->height, pict.data, pict.linesize);

                SDL_UnlockYUVOverlay(bmp);

                rect.x = 0;
                rect.y = 0;
                rect.w = pCodecCtx->width;
                rect.h = pCodecCtx->height;

                SDL_DisplayYUVOverlay(bmp, &rect);
                SDL_Delay(10);

            }
        }

        av_free_packet(&packet);
        SDL_PollEvent(&event);

        switch (event.type) {

            case SDL_QUIT:
                SDL_Quit();
                exit(0);
                break;

            default:
                break;
        }

    }


    av_free(pFrame);

    avcodec_close(pCodecCtx);

    avformat_close_input(&pFormatCtx);

    return 0;
}
