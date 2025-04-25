#include "thumbnailer.h"
#include <float.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/**
 * Potential thumbnail lookup filter to reduce the risk of an inappropriate
 * selection (such as a black frame) we could get with an absolute seek.
 *
 * Simplified version of algorithm by Vadim Zaliva <lord@crocodile.org>.
 * http://notbrainsurgery.livejournal.com/29773.html
 *
 * Adapted by Janis Petersons <bakape@gmail.com>
 */

#define HIST_SIZE 256
#define HIST_CHANNELS 3
#define MAX_FRAMES 10

// Compute sum-square deviation to estimate "closeness"
static double compute_error(const unsigned hist[HIST_SIZE][HIST_CHANNELS],
    const double average[HIST_SIZE][HIST_CHANNELS])
{
    double sum_sq_err = 0;
    for (int i = 0; i < HIST_SIZE; i++) {
        for (int j = 0; j < HIST_CHANNELS; j++) {
            const double err = average[i][j] - (double)hist[i][j];
            sum_sq_err += err * err;
        }
    }
    return sum_sq_err;
}

// Select best frame based on RGB histograms
static AVFrame* select_best_frame(AVFrame* frames[], int size)
{
    if (size == 1) {
        return frames[0];
    }

    // RGB color distribution histograms of the frames
    unsigned hists[MAX_FRAMES][HIST_SIZE][HIST_CHANNELS] = { 0 };

    // Compute each frame's histogram
    for (int frame_i = 0; frame_i < size; frame_i++) {
        const AVFrame* f = frames[frame_i];
        const int line_size = f->linesize[0];
        const uint8_t* p = f->data[0];
        for (int i = 0; i < f->height; i++) {
            const int offset = line_size * i;
            for (int j = 0; j < line_size; j++) {
                // Count amount of pixels in each channel.
                // Using modulo to account for frames in non-3-byte pixel
                // formats.
                hists[frame_i][p[offset + j]][j % HIST_CHANNELS]++;
            }
        }
    }

    // Average all histograms
    double average[HIST_SIZE][HIST_CHANNELS] = { 0 };
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < HIST_SIZE; j++) {
            // Unrolled for less data dependency
            average[i][0] += (double)hists[i][j][0];
            average[i][1] += (double)hists[i][j][1];
            average[i][2] += (double)hists[i][j][2];
        }
        // Unrolled for less data dependency
        average[i][0] /= size;
        average[i][1] /= size;
        average[i][2] /= size;
    }

    // Find the frame closer to the average using the sum of squared errors
    double min_sq_err = DBL_MAX;
    int best_i = 0;
    for (int i = 0; i < size; i++) {
        const double sq_err = compute_error(hists[i], average);
        if (sq_err < min_sq_err) {
            best_i = i;
            min_sq_err = sq_err;
        }
    }
    return frames[best_i];
}

// Calculate size and allocate buffer
static void alloc_buffer(struct Buffer* dst)
{
    dst->size
        = av_image_get_buffer_size(AV_PIX_FMT_RGBA, dst->width, dst->height, 32);
    dst->data = malloc(dst->size);
}

// Use point subsampling to scale image up to target size and convert to RGBA
static int resample(struct Buffer* dst, const AVFrame const* frame)
{
    struct SwsContext* ctx
        = sws_getContext(frame->width, frame->height, frame->format, dst->width,
            dst->height, AV_PIX_FMT_RGBA, SWS_POINT, NULL, NULL, NULL);
    if (!ctx) {
        return AVERROR(ENOMEM);
    }

    uint8_t* dst_data[1] = { dst->data }; // RGB have one plane
    int dst_linesize[1] = { 4 * dst->width }; // RGBA stride

    sws_scale(ctx, (const uint8_t* const*)frame->data, frame->linesize, 0,
        frame->height, dst_data, dst_linesize);

    sws_freeContext(ctx);
    return 0;
}

struct Pixel {
    // uint16_t fits the max value of 255 * 16
    uint16_t r, g, b, a;
};

// Downscale resampled image
static void downscale(struct Buffer* dst, const struct Buffer const* src)
{
    // First sum all pixels into a multidimensional array
    const size_t size = dst->height * dst->width * sizeof(struct Pixel);
    struct Pixel(*img)[dst->width] = malloc(size);
    memset(img, 0, size);

    int i = 0;
    for (int y = 0; y < src->height; y++) {
        const int dest_y = y ? y / 4 : 0;
        for (int x = 0; x < src->width; x++) {
            struct Pixel* p = &img[dest_y][x ? x / 4 : 0];

            // Skip pixels with maxed transparency
            const uint8_t alpha = src->data[i + 3];
            if (alpha != 0) {
                // Unrolled for less data dependency
                p->r += src->data[i];
                p->g += src->data[i + 1];
                p->b += src->data[i + 2];
                p->a += alpha;
            }

            i += 4; // Less data dependency than i++
        }
    }

    // Then average them and arrange as RGBA
    i = 0;
    for (int y = 0; y < dst->height; y++) {
        for (int x = 0; x < dst->width; x++) {
            const struct Pixel p = img[y][x];

            // Unrolled for less data dependency
            dst->data[i] = p.r ? p.r / 16 : 0;
            dst->data[i + 1] = p.g ? p.g / 16 : 0;
            dst->data[i + 2] = p.b ? p.b / 16 : 0;
            dst->data[i + 3] = p.a ? p.a / 16 : 0;
            i += 4;
        }
    }
}

// Decrease intensity of pixels with alpha
static void compensate_alpha(struct Buffer* img)
{
    int i = 0;
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            const uint8_t alpha = img->data[i + 3];
            if (alpha != 255) {
                const float scale = (float)alpha / (float)255;
                for (int j = 0; j < 3; j++) {
                    float val = (float)img->data[i + j] * scale;
                    if (val > 255) {
                        val = 255;
                    }
                    img->data[i + j] = (uint8_t)val;
                }
            }
            i += 4;
        }
    }
}

// Swap 2 RGBA pixels by memory addresses
static inline void swap_pixels(size_t a, size_t b)
{
    uint8_t tmp[4];
    memcpy(&tmp, (void*)a, 4);
    memcpy((void*)a, (void*)b, 4);
    memcpy((void*)b, tmp, 4);
}

static void mirror_horizontally(struct Buffer* img)
{
    for (size_t y = 0; y < img->height; y++) {
        size_t left = (size_t)img->data + y * img->width * 4;
        size_t right = left + (img->width - 1) * 4;
        for (size_t x = 0; x < img->width / 2; x++) {
            swap_pixels(left, right);
            left += 4;
            right -= 4;
        }
    }
}

static void mirror_vertically(struct Buffer* img)
{
    const size_t row_size = img->width * 4;

    for (size_t y = 0; y < img->height / 2; y++) {
        size_t top = (size_t)img->data + row_size * y;
        size_t bottom = (size_t)img->data + row_size * (img->height - 1 - y);
        for (size_t x = 0; x < img->width; x++) {
            swap_pixels(top, bottom);
            top += 4;
            bottom += 4;
        }
    }
}

// Rotate an image by 180 degrees
static void rotate_180(struct Buffer* img)
{
    const size_t row_size = img->width * 4;

    for (size_t y = 0; y < img->height / 2; y++) {
        size_t top = (size_t)img->data + row_size * y;
        size_t bottom = (size_t)img->data + row_size * (img->height - y) - 4;
        for (size_t x = 0; x < img->width; x++) {
            swap_pixels(top, bottom);
            top += 4;
            bottom -= 4;
        }
    }

    if (img->height % 2) {
        const size_t off = (size_t)img->data + row_size * (img->height / 2);
        for (int x = 0; x < img->width / 2; x++) {
            swap_pixels(off + x * 4, off + (img->width - x - 1) * 4);
        }
    }
}

// Swap buffers and dimensions after a 90 degree rotation in any direction
static void finish_90_rotation(struct Buffer* img, uint8_t* out)
{
    free(img->data);
    img->data = out;
    uint64_t tmp = img->width;
    img->width = img->height;
    img->height = tmp;
}

// Copy RGBA pixel from src to dst
static inline void copy_pixel(size_t dst, size_t src)
{
    memcpy((void*)dst, (void*)src, 4);
}

// Rotate an image by 90 degrees clockwise
static void rotate_90(struct Buffer* img)
{
    uint8_t* out = malloc(img->size);

    size_t src = (size_t)img->data;
    for (size_t y = 0; y < img->height; y++) {
        for (size_t x = 0; x < img->width; x++) {
            copy_pixel(
                (size_t)out + img->height * 4 * x + (img->height - y - 1) * 4,
                src);
            src += 4;
        }
    }

    finish_90_rotation(img, out);
}

// Rotate an image by 270 degrees clockwise
static void rotate_270(struct Buffer* img)
{
    uint8_t* out = malloc(img->size);

    size_t src = (size_t)img->data;
    for (size_t y = 0; y < img->height; y++) {
        for (size_t x = 0; x < img->width; x++) {
            copy_pixel(
                (size_t)out + img->height * 4 * (img->width - x - 1) + y * 4,
                src);
            src += 4;
        }
    }

    finish_90_rotation(img, out);
}

// Rotate or flip according to exif orientation as the thumbnail does not have
// any metadata
static void adjust_orientation(struct Buffer* img, const int orientation)
{
    switch (orientation) {
    case 2:
        mirror_horizontally(img);
        break;
    case 3:
        rotate_180(img);
        break;
    case 4:
        mirror_vertically(img);
        break;
    case 7:
        mirror_horizontally(img);
    case 6:
        rotate_90(img);
        break;
    case 5:
        mirror_horizontally(img);
    case 8:
        rotate_270(img);
        break;
    }
}

// Scale both image dimensions to fit in constraint, if it is exceeded
static void scale_dims(struct Buffer* img, uint64_t max, uint64_t val)
{
    if (val > max) {
        // Maintains aspect ratio
        const double scale = (double)val / (double)max;
        img->width = (uint64_t)((double)img->width / scale);
        img->height = (uint64_t)((double)img->height / scale);
    }
}

// Encode and scale frame to RGBA image
static int encode_frame(
    struct Buffer* img, AVFrame* frame, const struct Dims box, int orientation)
{
    int err;

    if (frame->metadata) {
        AVDictionaryEntry* e
            = av_dict_get(frame->metadata, "Orientation", NULL, 0);
        if (e) {
            orientation = atol(e->value);
        }
    }

    img->width = frame->width;
    img->height = frame->height;

    // If image fits inside thumbnail, simply convert to RGBA.
    //
    // scale_dims() does not work, if image size is exactly that of the target
    // thumbnail size. Perhaps a peculiarity of sws_scale().
    if (img->width < box.width && img->height < box.height) {
        alloc_buffer(img);
        err = resample(img, frame);
        if (err) {
            return err;
        }
        compensate_alpha(img);
        adjust_orientation(img, orientation);
        return 0;
    }

    scale_dims(img, box.width, img->width);
    scale_dims(img, box.height, img->height);

    // Subsample to 4 times the thumbnail size and then Box subsample that.
    // A decent enough compromise between quality and performance for images
    // around the thumbnail size and much bigger ones.
    struct Buffer enlarged
        = { .width = img->width * 4, .height = img->height * 4 };
    alloc_buffer(&enlarged);
    err = resample(&enlarged, frame);
    if (err) {
        free(enlarged.data);
        return err;
    }
    alloc_buffer(img);
    downscale(img, &enlarged);
    free(enlarged.data);
    adjust_orientation(img, orientation);
    return err;
}

// Read from stream until a full frame is read
static int read_frame(AVFormatContext* avfc, AVCodecContext* avcc,
    AVFrame* frame, const int stream)
{
    int err = 0;
    AVPacket pkt;

    // Continue until frame read
    while (1) {
        err = av_read_frame(avfc, &pkt);
        if (err) {
            goto end;
        }

        if (pkt.stream_index == stream) {
            err = avcodec_send_packet(avcc, &pkt);
            if (err < 0) {
                goto end;
            }

            err = avcodec_receive_frame(avcc, frame);
            switch (err) {
            case 0:
                goto end;
            case AVERROR(EAGAIN):
                av_packet_unref(&pkt);
                break;
            default:
                goto end;
            }
        }
    }

end:
    av_packet_unref(&pkt);
    return err;
}

int generate_thumbnail(struct Buffer* img, AVFormatContext* avfc,
    AVCodecContext* avcc, const int stream, const struct Dims thumb_dims)
{
    int err = 0;
    int size = 0;
    int i = 0;
    AVFrame* frames[MAX_FRAMES] = { NULL };
    AVFrame* next = NULL;

    // Read up to 10 frames
    while (1) {
        next = av_frame_alloc();
        err = read_frame(avfc, avcc, next, stream);
        if (err) {
            goto end;
        }

        // Analyze only every 3rd frame to cover a larger time frame
        if (!(i++ % 3)) {
            frames[size++] = next;
            next = NULL;
            if (size == MAX_FRAMES) {
                goto end;
            }
        } else {
            av_frame_free(&next);
        }
    }

end:
    if (size) {
        int orientation = 0;
        AVDictionaryEntry* e
            = av_dict_get(avfc->streams[stream]->metadata, "rotate", NULL, 0);
        if (e) {
            switch (atol(e->value)) {
            case 90:
                orientation = 6;
                break;
            case 180:
                orientation = 3;
                break;
            case 270:
                orientation = 8;
                break;
            }
        }

        // Ignore all read errors, if at least one frame read
        err = encode_frame(
            img, select_best_frame(frames, size), thumb_dims, orientation);
    }

    for (int i = 0; i < size; i++) {
        av_frame_free(&frames[i]);
    }
    if (next) {
        av_frame_free(&next);
    }

    return err;
}
