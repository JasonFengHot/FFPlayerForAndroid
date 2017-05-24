//
// Created by huibin on 4/19/17.
//

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <android/log.h>
#include <assert.h>
#include <SDL.h>

#define TAG "FFMPEG"

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

AVFormatContext *pFormatCtx = NULL;
AVCodecContext *pCodecCtx = NULL;
AVFrame *pFrame = NULL;
struct SwsContext *sws_ctx = NULL;
AVPacket packet;

int videoStream;
int frameFinished = 1;

Uint8 *yPlane, *uPlane, *vPlane;
size_t yPlaneSz, uvPlaneSz;
int uvPitch;

SDL_Renderer *renderer = NULL;
SDL_Texture *texture = NULL;
SDL_Event event;
SDL_Window *screen = NULL;

unsigned i;
AVCodecParameters *pCodecCtxOrig = NULL;
AVCodec *pCodec = NULL;

SDL_Event event;

char *filePath;

/**
 * audio
 */

int audioStream;
SDL_AudioSpec wanted_spec, spec;
AVCodecParameters *audioCodecCtxOrig = NULL;
AVCodecContext *audioCodecCtx = NULL;
AVCodec *audioCodec = NULL;

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;

int quit = 0;


void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

    AVPacketList *pkt1;
    if (av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;


    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {

        if (quit) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int audio_resampling(AVCodecContext *audio_decode_ctx,
                            AVFrame *audio_decode_frame,
                            enum AVSampleFormat out_sample_fmt,
                            int out_channels,
                            int out_sample_rate,
                            uint8_t *out_buf) {
    SwrContext *swr_ctx = NULL;
    int ret = 0;
    int64_t in_channel_layout = audio_decode_ctx->channel_layout;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_nb_channels = 0;
    int out_linesize = 0;
    int in_nb_samples = 0;
    int out_nb_samples = 0;
    int max_out_nb_samples = 0;
    uint8_t **resampled_data = NULL;
    int resampled_data_size = 0;

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        printf("swr_alloc error\n");
        return -1;
    }

    in_channel_layout = (audio_decode_ctx->channels ==
                         av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout)) ?
                        audio_decode_ctx->channel_layout :
                        av_get_default_channel_layout(audio_decode_ctx->channels);
    if (in_channel_layout <= 0) {
        printf("in_channel_layout error\n");
        return -1;
    }

    if (out_channels == 1) {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    } else if (out_channels == 2) {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    } else {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }

    in_nb_samples = audio_decode_frame->nb_samples;
    if (in_nb_samples <= 0) {
        printf("in_nb_samples error\n");
        return -1;
    }

    av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", audio_decode_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", audio_decode_ctx->sample_fmt, 0);

    av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

    if ((ret = swr_init(swr_ctx)) < 0) {
        printf("Failed to initialize the resampling context\n");
        return -1;
    }

    max_out_nb_samples = out_nb_samples = av_rescale_rnd(in_nb_samples,
                                                         out_sample_rate,
                                                         audio_decode_ctx->sample_rate,
                                                         AV_ROUND_UP);

    if (max_out_nb_samples <= 0) {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);

    ret = av_samples_alloc_array_and_samples(&resampled_data, &out_linesize, out_nb_channels,
                                             out_nb_samples,
                                             out_sample_fmt, 0);
    if (ret < 0) {
        printf("av_samples_alloc_array_and_samples error\n");
        return -1;
    }

    out_nb_samples = av_rescale_rnd(
            swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate) + in_nb_samples,
            out_sample_rate, audio_decode_ctx->sample_rate, AV_ROUND_UP);
    if (out_nb_samples <= 0) {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    if (out_nb_samples > max_out_nb_samples) {
        av_free(resampled_data[0]);
        ret = av_samples_alloc(resampled_data, &out_linesize, out_nb_channels, out_nb_samples,
                               out_sample_fmt, 1);
        max_out_nb_samples = out_nb_samples;
    }

    if (swr_ctx) {
        ret = swr_convert(swr_ctx, resampled_data, out_nb_samples,
                          (const uint8_t **) audio_decode_frame->data,
                          audio_decode_frame->nb_samples);
        if (ret < 0) {
            printf("swr_convert_error\n");
            return -1;
        }

        resampled_data_size = av_samples_get_buffer_size(&out_linesize, out_nb_channels, ret,
                                                         out_sample_fmt, 1);
        if (resampled_data_size < 0) {
            printf("av_samples_get_buffer_size error\n");
            return -1;
        }
    } else {
        printf("swr_ctx null error\n");
        return -1;
    }

    memcpy(out_buf, resampled_data[0], resampled_data_size);

    if (resampled_data) {
        av_freep(&resampled_data[0]);
    }
    av_freep(&resampled_data);
    resampled_data = NULL;

    if (swr_ctx) {
        swr_free(&swr_ctx);
    }
    return resampled_data_size;
}


int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {

    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    static AVFrame frame;

    int len1, data_size = 0;

    for (;;) {
        while (audio_pkt_size > 0) {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);
            if (len1 < 0) {
                /* if error, skip frame */
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;
            if (got_frame) {
                data_size = av_samples_get_buffer_size(NULL,
                                                       aCodecCtx->channels,
                                                       frame.nb_samples,
                                                       aCodecCtx->sample_fmt,
                                                       1);
                assert(data_size <= buf_size);
                memcpy(audio_buf, frame.data[0], data_size);

                data_size = audio_resampling(
                        audioCodecCtx,
                        &frame,
                        AV_SAMPLE_FMT_S16,
                        frame.channels,
                        frame.sample_rate,
                        audio_buf
                );
                assert(data_size <= buf_size);
            }


            if (data_size <= 0) {
                /* No data yet, get more frames */
                continue;
            }
            /* We have data, return it and come back for more later */
            return data_size;
        }
        if (pkt.data)
            av_free_packet(&pkt);

        if (quit) {
            return -1;
        }

        if (packet_queue_get(&audioq, &pkt, 1) < 0) {
            return -1;
        }
        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

    AVCodecContext *aCodecCtx = (AVCodecContext *) userdata;
    int len1, audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if (audio_size < 0) {
                /* If error, output silence */
                audio_buf_size = 1024; // arbitrary?
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *) audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

void video_thread() {
    int ret;
    ret = avcodec_send_packet(pCodecCtx, &packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        av_packet_unref(&packet);
    }

    ret = avcodec_receive_frame(pCodecCtx, pFrame);

    if (ret < 0 && ret != AVERROR_EOF) {
        av_packet_unref(&packet);
    }

    // Did we get a video frame?
    if (ret == 0) {
        AVFrame *pict = av_frame_alloc();
        pict->data[0] = yPlane;
        pict->data[1] = uPlane;
        pict->data[2] = vPlane;
        pict->linesize[0] = pCodecCtx->width;
        pict->linesize[1] = uvPitch;
        pict->linesize[2] = uvPitch;

        // Convert the image into YUV format that SDL uses
        sws_scale(sws_ctx, (uint8_t const *const *) pFrame->data,
                  pFrame->linesize, 0, pCodecCtx->height, pict->data,
                  pict->linesize);

        LOGD("video_thread");
        event.type = FF_REFRESH_EVENT;
        SDL_PushEvent(&event);
    }
}


void audio_thread() {

    packet_queue_put(&audioq, &packet);
}


int decode_thread() {
    while (av_read_frame(pFormatCtx, &packet) >= 0) {
        if (packet.stream_index == videoStream) {
            video_thread();
//            SDL_CreateThread(video_thread, "video_thread", NULL);
        } else if (packet.stream_index == audioStream) {
            audio_thread();
        }
    }
    event.type = FF_QUIT_EVENT;
    SDL_PushEvent(&event);
}


int init_thread() {
    // Open video file
    if (avformat_open_input(&pFormatCtx, filePath, NULL, NULL) != 0)
        return -1; // Couldn't open file

    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, filePath, 0);

    // Find the first video stream
    videoStream = -1;
    audioStream = -1;
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = i;
        }
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
        }
    }
    if (videoStream == -1) {
        return -1; // Didn't find a video stream
    }

    if (audioStream == -1) {
        return -1; // Didn't find a audio stream
    }

    /**
     * ---------------------audio------------------
     */
    audioCodecCtxOrig = pFormatCtx->streams[audioStream]->codecpar;
    audioCodec = avcodec_find_decoder(audioCodecCtxOrig->codec_id);
    if (audioCodec == NULL) {
        LOGE("Unsupported codec!\n");
        return -1; // Codec not found
    }

    // Get a pointer to the codec context for the video stream
    pCodecCtxOrig = pFormatCtx->streams[videoStream]->codecpar;
    // Find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
    if (pCodec == NULL) {
        LOGE("Unsupported codec!\n");
        return -1; // Codec not found
    }


    // Copy context
    audioCodecCtx = avcodec_alloc_context3(audioCodec);
    if (avcodec_parameters_to_context(audioCodecCtx, audioCodecCtxOrig) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1; // Error copying codec context
    }
    // Set audio settings from codec info
    wanted_spec.freq = audioCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = audioCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = audioCodecCtx;

    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }

    avcodec_open2(audioCodecCtx, audioCodec, NULL);

    // audio_st = pFormatCtx->streams[index]
    packet_queue_init(&audioq);
    SDL_PauseAudio(0);
    /**
     * ------------------------------------------------------
     */

    // Copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (avcodec_parameters_to_context(pCodecCtx, pCodecCtxOrig) != 0) {
        LOGE("Couldn't copy codec context");
        return -1; // Error copying codec context
    }

    // Open codec
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
        return -1; // Could not open codec

    // Allocate video frame
    pFrame = av_frame_alloc();

    // Make a screen to put our video
    screen = SDL_CreateWindow(
            "FFmpeg Tutorial",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            400,
            400,
            0
    );

    if (!screen) {
        LOGE("SDL: could not create window - exiting\n");
        exit(1);
    }

    renderer = SDL_CreateRenderer(screen, -1, 0);
    if (!renderer) {
        LOGE("SDL: could not create renderer - exiting\n");
        exit(1);
    }


    LOGD("screen final size: %dx%d\n", pCodecCtx->width, pCodecCtx->height);
    // Allocate a place to put our YUV image on that screen
    texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_YV12,
            SDL_TEXTUREACCESS_STREAMING,
            pCodecCtx->width,
            pCodecCtx->height
    );
    if (!texture) {
        LOGE("SDL: could not create texture - exiting\n");
        exit(1);
    }

    // initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
                             pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
                             AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR,
                             NULL,
                             NULL,
                             NULL);

    // set up YV12 pixel array (12 bits per pixel)
    yPlaneSz = pCodecCtx->width * pCodecCtx->height;
    uvPlaneSz = pCodecCtx->width * pCodecCtx->height / 4;
    yPlane = (Uint8 *) malloc(yPlaneSz);
    uPlane = (Uint8 *) malloc(uvPlaneSz);
    vPlane = (Uint8 *) malloc(uvPlaneSz);
    if (!yPlane || !uPlane || !vPlane) {
        LOGE("Could not allocate pixel buffers - exiting\n");
        exit(1);
    }

    uvPitch = pCodecCtx->width / 2;
    SDL_CreateThread(decode_thread, "video_thread", NULL);


}

int ffplay(char *path) {

    filePath = path;

    // Register all formats and codecs
    av_register_all();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        LOGE("Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    init_thread();
//    SDL_CreateThread(decode_thread, "decode_thread", NULL);

    for (;;) {
        SDL_WaitEvent(&event);
        switch (event.type) {
            case FF_QUIT_EVENT:
                // Free the YUV frame
                av_frame_free(&pFrame);
                free(yPlane);
                free(uPlane);
                free(vPlane);

                // Close the codec
                avcodec_close(pCodecCtx);
                avcodec_parameters_free(pCodecCtxOrig);
                // Close the video file
                avformat_close_input(&pFormatCtx);


                SDL_DestroyTexture(texture);
                SDL_DestroyRenderer(renderer);
                SDL_DestroyWindow(screen);
                SDL_Quit();
                exit(0);
            case FF_REFRESH_EVENT:
                SDL_UpdateYUVTexture(
                        texture,
                        NULL,
                        yPlane,
                        pCodecCtx->width,
                        uPlane,
                        uvPitch,
                        vPlane,
                        uvPitch
                );
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
                break;
            default:
                break;
        }
    }
}