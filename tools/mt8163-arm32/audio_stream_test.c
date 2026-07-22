/*
 * Bounded direct-UAPI audio stream test for the LibreEcho ARM32 image.
 *
 * This intentionally avoids mixer controls.  It configures one PCM endpoint,
 * emits a low-level sine tone or captures samples for a short interval, then
 * drops the stream and reports the result.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

#define PLAY_RATE 48000U
#define PLAY_CHANNELS 2U
#define PLAY_PERIOD 1024U
#define PLAY_PERIODS 4U
#define CAPTURE_RATE 16000U
#define CAPTURE_CHANNELS 9U
#define CAPTURE_PERIOD 256U
#define CAPTURE_PERIODS 10U
#define MAX_SECONDS 5U
#define POLL_MS 250

static void report_errno(const char *op, const char *path)
{
    fprintf(stderr, "AUDIO_STREAM_ERROR operation=%s path=%s errno=%d (%s)\n",
            op, path, errno, strerror(errno));
}

static unsigned long long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static struct snd_mask *hw_mask(struct snd_pcm_hw_params *params, unsigned int p)
{
    return &params->masks[p - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *hw_interval(struct snd_pcm_hw_params *params,
                                         unsigned int p)
{
    return &params->intervals[p - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void hw_any(struct snd_pcm_hw_params *params)
{
    unsigned int i;

    memset(params, 0, sizeof(*params));
    for (i = 0; i < sizeof(params->masks) / sizeof(params->masks[0]); i++)
        memset(&params->masks[i], 0xff, sizeof(params->masks[i]));
    for (i = 0; i < sizeof(params->intervals) /
                  sizeof(params->intervals[0]); i++) {
        params->intervals[i].min = 0;
        params->intervals[i].max = UINT_MAX;
    }
    params->rmask = UINT_MAX;
}

static void hw_set_mask(struct snd_pcm_hw_params *params, unsigned int p,
                        unsigned int value)
{
    struct snd_mask *mask = hw_mask(params, p);

    memset(mask, 0, sizeof(*mask));
    mask->bits[value / 32U] |= 1U << (value % 32U);
}

static void hw_set_interval(struct snd_pcm_hw_params *params, unsigned int p,
                            unsigned int value)
{
    struct snd_interval *interval = hw_interval(params, p);

    interval->min = value;
    interval->max = value;
    interval->openmin = 0;
    interval->openmax = 0;
    interval->integer = 1;
    interval->empty = 0;
}

static int configure_pcm(int fd, const char *path, unsigned int format,
                         unsigned int channels, unsigned int rate,
                         unsigned int period, unsigned int periods)
{
    struct snd_pcm_hw_params params;

    hw_any(&params);
    hw_set_mask(&params, SNDRV_PCM_HW_PARAM_ACCESS,
                SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_set_mask(&params, SNDRV_PCM_HW_PARAM_FORMAT, format);
    hw_set_mask(&params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                SNDRV_PCM_SUBFORMAT_STD);
    hw_set_interval(&params, SNDRV_PCM_HW_PARAM_CHANNELS, channels);
    hw_set_interval(&params, SNDRV_PCM_HW_PARAM_RATE, rate);
    hw_set_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
    hw_set_interval(&params, SNDRV_PCM_HW_PARAM_PERIODS, periods);
    hw_set_interval(&params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
                    period * periods);

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0) {
        report_errno("hw-refine", path);
        return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
        report_errno("hw-params", path);
        return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        report_errno("prepare", path);
        return -1;
    }
    return 0;
}

static int transfer(int fd, const char *path, int stream, void *buffer,
                    unsigned int frames, unsigned long long deadline,
                    unsigned long long *total)
{
    struct snd_xferi xfer;

    for (;;) {
        int rc;

        memset(&xfer, 0, sizeof(xfer));
        xfer.buf = buffer;
        xfer.frames = frames;
        if (stream == SNDRV_PCM_STREAM_PLAYBACK)
            rc = ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
        else
            rc = ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
        if (rc == 0 && xfer.result > 0) {
            *total += (unsigned long long)xfer.result;
            return 0;
        }
        if (rc == 0) {
            errno = EIO;
            report_errno("transfer-zero", path);
            return -1;
        }
        if (errno != EAGAIN && errno != EINTR) {
            report_errno(stream == SNDRV_PCM_STREAM_PLAYBACK ?
                         "writei" : "readi", path);
            return -1;
        }
        if (now_ms() >= deadline) {
            errno = ETIMEDOUT;
            report_errno("transfer-timeout", path);
            return -1;
        }
        {
            struct pollfd pfd;

            pfd.fd = fd;
            pfd.events = stream == SNDRV_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, POLL_MS) < 0 && errno != EINTR) {
                report_errno("poll", path);
                return -1;
            }
        }
    }
}

static int run_playback(unsigned int card, unsigned int device,
                        unsigned int seconds)
{
    const unsigned int frames = PLAY_PERIOD;
    const unsigned int bytes = frames * PLAY_CHANNELS * sizeof(int16_t);
    const double phase_step = 2.0 * M_PI * 440.0 / (double)PLAY_RATE;
    char path[64];
    int16_t *buffer;
    unsigned int i;
    unsigned long long total = 0;
    unsigned long long deadline;
    double phase = 0.0;
    int fd;
    int rc = 1;

    snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%up", card, device);
    fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        report_errno("open-playback", path);
        return 1;
    }
    buffer = calloc(1, bytes);
    if (!buffer) {
        fprintf(stderr, "AUDIO_STREAM_ERROR operation=alloc bytes=%u\n", bytes);
        goto out;
    }
    for (i = 0; i < frames; i++) {
        int16_t sample = (int16_t)(3000.0 * sin(phase));

        buffer[i * 2] = sample;
        buffer[i * 2 + 1] = sample;
        phase += phase_step;
        if (phase >= 2.0 * M_PI)
            phase -= 2.0 * M_PI;
    }
    if (configure_pcm(fd, path, SNDRV_PCM_FORMAT_S16_LE, PLAY_CHANNELS,
                      PLAY_RATE, PLAY_PERIOD, PLAY_PERIODS) < 0)
        goto free_buffer;
    deadline = now_ms() + (unsigned long long)seconds * 1000ULL;
    if (transfer(fd, path, SNDRV_PCM_STREAM_PLAYBACK, buffer, frames,
                 deadline, &total) < 0)
        goto drop;
    if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) {
        report_errno("start-playback", path);
        goto drop;
    }
    while (now_ms() < deadline) {
        if (transfer(fd, path, SNDRV_PCM_STREAM_PLAYBACK, buffer, frames,
                     deadline, &total) < 0)
            goto drop;
    }
    printf("AUDIO_PLAYBACK_RESULT=PASS path=%s rate=%u channels=%u "
           "frames=%llu seconds=%u\n", path, PLAY_RATE, PLAY_CHANNELS,
           total, seconds);
    rc = 0;
drop:
    ioctl(fd, SNDRV_PCM_IOCTL_DROP);
free_buffer:
    free(buffer);
out:
    close(fd);
    return rc;
}

static int32_t sample_s24_3le(const unsigned char *p)
{
    int32_t value = (int32_t)p[0] | ((int32_t)p[1] << 8) |
                    ((int32_t)p[2] << 16);

    if (value & 0x00800000)
        value |= (int32_t)0xff000000;
    return value;
}

static int run_capture(unsigned int card, unsigned int device,
                       unsigned int seconds)
{
    const unsigned int frames = CAPTURE_PERIOD;
    const unsigned int bytes = frames * CAPTURE_CHANNELS * 3U;
    char path[64];
    unsigned char *buffer;
    unsigned long long total = 0;
    unsigned long long nonzero = 0;
    int32_t minimum = INT_MAX;
    int32_t maximum = INT_MIN;
    unsigned long long deadline;
    int fd;
    int rc = 1;

    snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%uc", card, device);
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        report_errno("open-capture", path);
        return 1;
    }
    buffer = calloc(1, bytes);
    if (!buffer) {
        fprintf(stderr, "AUDIO_STREAM_ERROR operation=alloc bytes=%u\n", bytes);
        goto out;
    }
    if (configure_pcm(fd, path, SNDRV_PCM_FORMAT_S24_3LE, CAPTURE_CHANNELS,
                      CAPTURE_RATE, CAPTURE_PERIOD, CAPTURE_PERIODS) < 0)
        goto free_buffer;
    if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) {
        report_errno("start-capture", path);
        goto drop;
    }
    deadline = now_ms() + (unsigned long long)seconds * 1000ULL;
    while (now_ms() < deadline) {
        unsigned int i;

        if (transfer(fd, path, SNDRV_PCM_STREAM_CAPTURE, buffer, frames,
                     deadline, &total) < 0)
            goto drop;
        for (i = 0; i < frames * CAPTURE_CHANNELS; i++) {
            int32_t value = sample_s24_3le(buffer + i * 3U);

            if (value != 0)
                nonzero++;
            if (value < minimum)
                minimum = value;
            if (value > maximum)
                maximum = value;
        }
    }
    printf("AUDIO_CAPTURE_RESULT=PASS path=%s rate=%u channels=%u "
           "frames=%llu nonzero_samples=%llu min=%d max=%d seconds=%u\n",
           path, CAPTURE_RATE, CAPTURE_CHANNELS, total, nonzero, minimum,
           maximum, seconds);
    rc = 0;
drop:
    ioctl(fd, SNDRV_PCM_IOCTL_DROP);
free_buffer:
    free(buffer);
out:
    close(fd);
    return rc;
}

static int parse_uint(const char *text, unsigned int *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *text == '\0' || *end != '\0' || parsed > UINT_MAX)
        return -1;
    *value = (unsigned int)parsed;
    return 0;
}

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s play CARD DEVICE SECONDS | "
            "%s capture CARD DEVICE SECONDS\n", name, name);
}

int main(int argc, char **argv)
{
    unsigned int card;
    unsigned int device;
    unsigned int seconds;

    if (argc != 5 || parse_uint(argv[2], &card) < 0 ||
        parse_uint(argv[3], &device) < 0 || parse_uint(argv[4], &seconds) < 0 ||
        seconds == 0 || seconds > MAX_SECONDS) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "play"))
        return run_playback(card, device, seconds);
    if (!strcmp(argv[1], "capture"))
        return run_capture(card, device, seconds);
    usage(argv[0]);
    return 2;
}
