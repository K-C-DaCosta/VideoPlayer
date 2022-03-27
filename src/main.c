#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <math.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define MIN(a, b) ((((a) < (b)) ? (a) : (b)))
#define MAX(a, b) ((((a) > (b)) ? (a) : (b)))

/// Walk through streams until stream with matching `type` is found
int find_stream_by_type(AVFormatContext *pFormatCtx, enum AVMediaType type)
{
    for (int k = 0; k < pFormatCtx->nb_streams; k++)
    {
        AVStream *stream = pFormatCtx->streams[k];
        if (stream->codec->codec_type == type)
        {
            return k;
        }
    }
    return -1;
}

// returns 0 on success and -1 if operation failed
// pFrame assumed to be rgb888
int copy_avframe_to_sdltexture(AVFrame *pFrame, int width, int height, SDL_Texture *tex)
{
    return SDL_UpdateTexture(tex, NULL, pFrame->data[0], width * 3);
}

int GI(int x, int y, int width)
{
    return y * width + x;
}

typedef struct _RingBuffer
{
    // maximum number of elements in the ring buffer
    int capacity;
    // current number of elements enqueued in the ring buffers
    int len;
    //element size in bytes
    int elem_size;
    //pointers for the queue
    int front, rear;
    //arbitrary data
    void *buffer;
} RingBuffer;

/*
Creates a ring buffer. This buffer does a one time allocation, so it doesn't grow. 
@param capacity the total number of elements in the buffer 
@param element_size  the number of bytes in each element
*/
RingBuffer rbCreate(int capacity, int element_size)
{
    RingBuffer rb = {
        capacity,
        0,
        element_size,
        0,
        0,
        calloc(capacity, element_size)};
    return rb;
}
/*
returns true if full and false otherwise
*/
bool rbIsFull(RingBuffer *rb)
{
    return rb->len >= rb->capacity;
}
/*
returns true if full and false otherwise
*/
bool rbIsEmpty(RingBuffer *rb)
{
    return rb->len <= 0;
}

/*
pops the rear of the queue. 
@returns if successful the positive index of the popped item otherwise -1 
*/
int rbPopRear(RingBuffer *rb)
{
    if (rbIsEmpty(rb))
    {
        return -1;
    }
    else
    {
        int old_rear = (rb->rear + rb->capacity - 1) % rb->capacity;
        rb->rear = old_rear;
        rb->len -= 1;
        return old_rear;
    }
}
/*
Enques item into the buffer
@returns the index and the newly enqued item and -1 if empty.
*/
int rbEnqueue(RingBuffer *rb)
{
    if (rbIsFull(rb))
    {
        return -1;
    }
    else
    {
        int old_rear = rb->rear;
        rb->rear = (rb->rear + 1) % rb->capacity;
        rb->len += 1;
        return old_rear;
    }
}

void rbClear(RingBuffer *rb)
{
    rb->len = 0;
    rb->front = 0;
    rb->rear = 0;
}
/*
Deques item off of the buffer
@returns the index and the recently dequeued item and -1 if empty.
*/
int rbDequeue(RingBuffer *rb)
{
    if (rbIsEmpty(rb))
    {
        return -1;
    }
    else
    {
        int old_front = rb->front;
        rb->front = (rb->front + 1) % rb->capacity;
        rb->len -= 1;
        return old_front;
    }
}

typedef struct _AudioSampleBuffer
{
    int capacity;
    int len;
    int16_t *buffer;
} AudioSampleBuffer;

AudioSampleBuffer *asbCreateBox(int max_capacity)
{
    AudioSampleBuffer *asb = malloc(sizeof(AudioSampleBuffer));
    asb->capacity = max_capacity;
    asb->len = 0;
    asb->buffer = calloc(max_capacity, sizeof(int16_t));
    return asb;
}

void asbFreeBox(AudioSampleBuffer *asb)
{
    asbFree(asb);
    free(asb);
}

void asbFree(AudioSampleBuffer *asb)
{
    free(asb->buffer);
}

typedef struct _AudioState
{
    RingBuffer audio_sample_buffer_queue;
    SDL_mutex *lock;
    SDL_cond *cond_var;
    SDL_mutex *cond_guard;

    int channels;
    int pointer;
    int frequency;
    float time;
} AudioState;

AudioState audstateCreate(int channels, int frequency)
{
    AudioState as;
    as.audio_sample_buffer_queue = rbCreate(128, sizeof(AudioSampleBuffer *));
    as.lock = SDL_CreateMutex();
    as.channels = channels;
    as.frequency = frequency;
    as.pointer = 0;
    as.time = 0.0f;
    as.cond_var = SDL_CreateCond();
    as.cond_guard = SDL_CreateMutex();

    AudioSampleBuffer **asb_buffer = as.audio_sample_buffer_queue.buffer;
    //allocate sample bufffers
    for (int k = 0; k < as.audio_sample_buffer_queue.capacity; k++)
    {
        asb_buffer[k] = asbCreateBox(4096);
    }
    return as;
}

void audio_callback(void *userdata, Uint8 *stream, int len_bytes)
{

    AudioState *state = (AudioState *)userdata;
    int samp_buf_index = -1;
    int16_t *out = stream;
    int16_t out_len = len_bytes / sizeof(int16_t);

    memset(stream, 0, len_bytes);

    SDL_LockMutex(state->lock);

    if (rbIsEmpty(&state->audio_sample_buffer_queue))
    {
        printf("player state buffer empty !\n");
        SDL_UnlockMutex(state->lock);
        //freeze thread and wait until more data arrives
        SDL_LockMutex(state->cond_guard);
        SDL_CondWaitTimeout(state->cond_var, state->cond_guard, 500);
    }
    else
    {
        printf("buffer not empty [size = %d front=%d , rear=%d]\n",
               state->audio_sample_buffer_queue.len,
               state->audio_sample_buffer_queue.front,
               state->audio_sample_buffer_queue.rear);
        SDL_UnlockMutex(state->lock);
    }

    SDL_LockMutex(state->lock);

    if ((samp_buf_index = rbDequeue(&state->audio_sample_buffer_queue)) >= 0)
    {

        AudioSampleBuffer *input_samples = ((AudioSampleBuffer **)state->audio_sample_buffer_queue.buffer)[samp_buf_index];
        int in_len = input_samples->len;
        int num_channels = state->channels;

        // printf("channels = %d, input  len = %d\noutput len = %d\n", num_channels, in_len, out_len);

        // samples are non-planar and come in groups per channel
        // in stereo, samples are of the form: L_SAMP_0 R_SAMP_0 L_SAMP_1 R_SAMP_1 ... L_SAMP_N-1 R_SAMP_N-1 ...
        int in_len_per_channel = (in_len / num_channels);
        int out_len_per_channel = (out_len / num_channels);

        float scale = ((float)(in_len_per_channel)) / ((float)(out_len_per_channel));

        for (int k = 0; k < out_len_per_channel; k++)
        {
            float group_index_approx = k * scale;
            int int_part = floorf(group_index_approx);
            int fract_part = group_index_approx - floorf(group_index_approx);
            int g0 = int_part;
            int g1 = MIN(int_part + 1, in_len_per_channel - 1);
            for (int i = 0; i < num_channels; i++)
            {
                float s0 = input_samples->buffer[num_channels * g0 + i];
                float s1 = input_samples->buffer[num_channels * g1 + i];
                float lerp = ((s1 - s0) * fract_part + s0);
                out[num_channels * k + i] = lerp;
            }
        }
    }

    SDL_UnlockMutex(state->lock);
}

int main(int argc, char *argv[])
{
    AVFormatContext *pFormatCtx = NULL;

    AVCodecContext *pCodecVideoCtxOrig = NULL,
                   *pCodecVideoCtx = NULL,
                   *pCodecAudioCtxOrig = NULL,
                   *pCodecAudioCtx = NULL;

    AVCodec *pVideoCodec = NULL,
            *pAudioCodec = NULL;

    AVFrame *pFrameRGB = NULL,
            *pFrame = NULL,
            *aFrame = NULL;

    struct SwsContext *sws_ctx = NULL;
    AVPacket packet;

    int video_stream_index = -1,
        audio_stream_index = -1,
        frame_finished = -1,
        num_bytes = -1;

    bool is_window_running = true;

    SDL_Event event;
    SDL_Window *window = NULL;

    if (argc < 2)
    {
        printf("Error:Media file argument not supplied.\n");
        return -1;
    }

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    av_register_all();

    // Open video file
    if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
    {
        printf("Couldn't open the file\n");
        return -1; // Couldn't open file
    }

    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1; // Couldn't find stream information

    av_dump_format(pFormatCtx, 0, argv[1], 0);

    //find, setup and open video decoder
    if ((video_stream_index = find_stream_by_type(pFormatCtx, AVMEDIA_TYPE_VIDEO)) == -1)
    {
        printf("Video stream not found\n");
        return -1;
    }

    pCodecVideoCtxOrig = pFormatCtx->streams[video_stream_index]->codec;
    if ((pVideoCodec = avcodec_find_decoder(pCodecVideoCtxOrig->codec_id)) == NULL)
    {
        printf("Unsupported codec!, No video decoder found\n");
        return -1;
    }

    pCodecVideoCtx = avcodec_alloc_context3(pVideoCodec);
    if (avcodec_copy_context(pCodecVideoCtx, pCodecVideoCtxOrig) != 0)
    {
        printf("Couldn't copy video codec\n");
        return -1;
    }

    if (avcodec_open2(pCodecVideoCtx, pVideoCodec, NULL) < 0)
    {
        printf("cound not open video codec\n");
        return -1;
    }

    //allocated video frames
    pFrameRGB = av_frame_alloc();
    pFrame = av_frame_alloc();

    num_bytes = avpicture_get_size(AV_PIX_FMT_RGB24, pCodecVideoCtx->width, pCodecVideoCtx->height);
    uint8_t *buffer = av_malloc(num_bytes);
    avpicture_fill((AVPicture *)pFrameRGB, buffer, AV_PIX_FMT_RGB24, pCodecVideoCtx->width, pCodecVideoCtx->height);

    sws_ctx = sws_getContext(pCodecVideoCtx->width,
                             pCodecVideoCtx->height,
                             pCodecVideoCtx->pix_fmt,
                             pCodecVideoCtx->width,
                             pCodecVideoCtx->height,
                             AV_PIX_FMT_RGB24,
                             SWS_FAST_BILINEAR,
                             NULL,
                             NULL,
                             NULL);

    //find, setup and open audio decoder
    if ((audio_stream_index = find_stream_by_type(pFormatCtx, AVMEDIA_TYPE_AUDIO)) == -1)
    {
        printf("Audio stream not found\n");
        return -1;
    }

    pCodecAudioCtxOrig = pFormatCtx->streams[audio_stream_index]->codec;
    if ((pAudioCodec = avcodec_find_decoder(pCodecAudioCtxOrig->codec_id)) == NULL)
    {
        printf("Unsupported codec!, No audio decoder found\n");
        return -1;
    }

    pCodecAudioCtx = avcodec_alloc_context3(pAudioCodec);
    if (avcodec_copy_context(pCodecAudioCtx, pCodecAudioCtxOrig) != 0)
    {
        printf("Couldn't copy audio codex\n");
        return -1;
    }

    if (avcodec_open2(pCodecAudioCtx, pAudioCodec, NULL) < 0)
    {
        printf("cound not open audio codec\n");
        return -1;
    }

    // prepare resampler
    struct SwrContext *swr = swr_alloc();
    av_opt_set_int(swr, "in_channel_count", pCodecAudioCtx->channels, 0);
    av_opt_set_int(swr, "out_channel_count", 2, 0);
    av_opt_set_int(swr, "in_channel_layout", pCodecAudioCtx->channel_layout, 0);
    av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_int(swr, "in_sample_rate", pCodecAudioCtx->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", pCodecAudioCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", pCodecAudioCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(swr);

    if (swr_is_initialized(swr) == 0)
    {
        printf("Resampler has not been properly initalized\n");
        return -1;
    }

    aFrame = av_frame_alloc();

    AudioState audio_state = audstateCreate(pCodecAudioCtx->channels, pCodecAudioCtx->sample_rate);
    SDL_AudioSpec desired_spec,
        obtained_spec;

    printf("queue cap = %d\n", audio_state.audio_sample_buffer_queue.capacity);
    AudioSampleBuffer *asb = ((AudioSampleBuffer **)audio_state.audio_sample_buffer_queue.buffer)[0];
    printf("asb addr = %x\n", asb);

    // printf("channels = %d\n", pCodecAudioCtx->channels);
    //fill out form for desired specs
    desired_spec.freq = pCodecAudioCtx->sample_rate;
    desired_spec.format = AUDIO_S16;
    desired_spec.channels = pCodecAudioCtx->channels;
    desired_spec.silence = 0;
    desired_spec.samples = 2048;
    desired_spec.callback = audio_callback;
    desired_spec.userdata = &audio_state;

    if (SDL_OpenAudio(&desired_spec, &obtained_spec) < 0)
    {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow("video_player",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              pCodecVideoCtx->width,
                              pCodecVideoCtx->height,
                              SDL_WINDOW_RESIZABLE);

    uint32_t format = SDL_GetWindowPixelFormat(window);
    printf("The window format is: %s\n", SDL_GetPixelFormatName(format));

    //libavcoded represents framerate as a rational number
    //here is the numerator of the framerate
    int fr_num = pCodecVideoCtx->framerate.num;

    //here is the denominator of the framerate
    int fr_denom = pCodecVideoCtx->framerate.den;
    printf("framerate = [[%d/%d]]\n", fr_num, fr_denom);

    SDL_Renderer *rend = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    int mx, my;

    int t0 = SDL_GetTicks();
    int wait_time = 0;
    bool frame_decoded = false;


    // int delay_coef = 1000; 

    RingBuffer frame_buffer = rbCreate(8, sizeof(SDL_Texture *));

    RingBuffer pcm_ring_buffer = rbCreate(1 << 14, sizeof(uint16_t));

    for (int k = 0; k < frame_buffer.capacity; k++)
    {
        ((SDL_Texture **)frame_buffer.buffer)[k] = SDL_CreateTexture(rend,
                                                                     SDL_PIXELFORMAT_RGB24,
                                                                     SDL_TEXTUREACCESS_STREAMING,
                                                                     pCodecVideoCtx->width,
                                                                     pCodecVideoCtx->height);
    }

    int16_t *resampled_decoded_pcm = NULL;

    //controlls the amount of samples sent to SDL backend
    int enqueue_quota = desired_spec.samples;

    //allocate enough memory to handle any sample size
    av_samples_alloc((uint8_t **)&resampled_decoded_pcm, NULL, 2, 1 << 14, AV_SAMPLE_FMT_S16, 0);

    SDL_PauseAudio(0);

    while (is_window_running)
    {
        // get mouse position
        SDL_GetMouseState(&mx, &my);

        // printf("frame ring buffer len = %d\n",frame_buffer.len);

        // parse event stream
        while (SDL_PollEvent(&event) != 0)
        {
            if (event.type == SDL_QUIT)
            {
                is_window_running = false;
            }

            if (event.type == SDL_WINDOWEVENT)
            {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED)
                {

                    SDL_Window *win = NULL;
                    if ((win = SDL_GetWindowFromID(event.window.windowID)) != NULL)
                    {
                    }
                }
            }
        }

        //if audio buffer is running low, drop some frames to continue buffering
        SDL_LockMutex(audio_state.lock);

        if (rbIsFull(&frame_buffer) && audio_state.audio_sample_buffer_queue.len <= 1)
        {
            rbClear(&frame_buffer);
        }

        SDL_UnlockMutex(audio_state.lock);

        if (rbIsFull(&frame_buffer) == false)
        {

            if (av_read_frame(pFormatCtx, &packet) >= 0)
            {
                if (packet.stream_index == video_stream_index)
                {
                    avcodec_decode_video2(pCodecVideoCtx, pFrame, &frame_finished, &packet);

                    if (frame_finished)
                    {
                        sws_scale(sws_ctx,
                                  (uint8_t const *const *)pFrame->data,
                                  pFrame->linesize,
                                  0,
                                  pCodecVideoCtx->height,
                                  pFrameRGB->data,
                                  pFrameRGB->linesize);

                        int enqueue_index = rbEnqueue(&frame_buffer);
                        SDL_Texture *enqueued_texture = ((SDL_Texture **)frame_buffer.buffer)[enqueue_index];
                        copy_avframe_to_sdltexture(pFrameRGB, pCodecVideoCtx->width, pCodecVideoCtx->height, enqueued_texture);
                    }
                }
                else if (packet.stream_index == audio_stream_index)
                {
                    int samps_read = avcodec_decode_audio4(pCodecAudioCtx, aFrame, &frame_finished, &packet);
                    // printf("samps read = %d\n", samps_read);
                    // packet.size

                    if (frame_finished)
                    {
                        bool can_enqueue = false;

                        SDL_LockMutex(audio_state.lock);
                        can_enqueue = rbIsFull(&audio_state.audio_sample_buffer_queue) == false;
                        SDL_UnlockMutex(audio_state.lock);

                        if (can_enqueue)
                        {
                            // convert pcm stream to desired PCM format
                            int converted_samples_per_channel = swr_convert(swr,
                                                                            (uint8_t **)&resampled_decoded_pcm,
                                                                            aFrame->nb_samples,
                                                                            (uint8_t **)aFrame->data,
                                                                            aFrame->nb_samples);

                            //fill a ring_buffer filled with decoded samples
                            // I multiply convertex_samples_per_channel by two because swr converts to stereo(interleaved)
                            for (int index = -1, k = 0; k < converted_samples_per_channel * 2; k++)
                            {
                                int16_t decoded_sample = resampled_decoded_pcm[k];
                                if ((index = rbEnqueue(&pcm_ring_buffer)) >= 0)
                                {
                                    ((int16_t *)pcm_ring_buffer.buffer)[index] = decoded_sample;
                                }
                            }

                            SDL_LockMutex(audio_state.lock);

                            /// When the ring buffer has enough to send over we enqueue a buffer
                            /// that is shared with the audio player
                            if (pcm_ring_buffer.len >= enqueue_quota)
                            {
                                int index = rbEnqueue(&audio_state.audio_sample_buffer_queue);
                                AudioSampleBuffer *samp_buffer = ((AudioSampleBuffer **)audio_state.audio_sample_buffer_queue.buffer)[index];
                                samp_buffer->len = enqueue_quota;
                                for (int k = 0; k < enqueue_quota; k++)
                                {
                                    int dequed_sample_index = rbDequeue(&pcm_ring_buffer);
                                    samp_buffer->buffer[k] = ((int16_t *)pcm_ring_buffer.buffer)[dequed_sample_index];
                                }

                                SDL_CondBroadcast(audio_state.cond_var);
                            }
                            SDL_UnlockMutex(audio_state.lock);
                        }
                    }
                }
                // Free the packet that was allocated by av_read_frame
                av_free_packet(&packet);
            }
        }

        // display every frame at specified framerate
        // notice that:
        // 'dt' => is elapsed time starting from the last frame displayed
        // '1000*(fr_denom/fr_num)' => is the period converted from seconds to milliseconds
        // 'fr_num*dt >= 1000*fr_denom'  <=>  dt >= 1/FRAMERATE <=>  dt >= PERIOD  <=>   dt >= 1000*(fr_denom/fr_num)
        int dt = SDL_GetTicks() - t0;
        if (fr_num * dt >= 1000 * fr_denom)
        {
            //NOTE:every line in this if-statement executes at the video's framerate

            if (rbIsEmpty(&frame_buffer) == false)
            {
                //consume and display a frame from the the frame_buffer queue
                int index = rbDequeue(&frame_buffer);
                SDL_RenderCopy(rend, ((SDL_Texture **)frame_buffer.buffer)[index], NULL, NULL);
                SDL_RenderPresent(rend);
            }

            // printf("pcm_buffer_len = %d\n",pcm_ring_buffer.len);

            t0 = SDL_GetTicks();
        }
    }

    // Free the RGB image
    av_free(buffer);
    av_free(pFrameRGB);

    // Free the YUV frame
    av_free(pFrame);

    // Close the codecs
    avcodec_close(pCodecVideoCtx);
    avcodec_close(pCodecVideoCtxOrig);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    // SDL_DestoryRenderer(rend);
    SDL_Quit();

    return 0;
}
