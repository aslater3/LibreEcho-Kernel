/*
 * Minimal ARM32 ALSA control/PCM probe for the LibreEcho bring-up image.
 *
 * This deliberately uses the kernel UAPI directly: it has no dependency on
 * ALSA userspace libraries and only observes card/PCM capabilities.  The
 * probe command opens one PCM endpoint and runs HW_REFINE, but does not start
 * playback/capture or change mixer state.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

static void field_text(char *out, size_t out_size,
                       const unsigned char *in, size_t in_size)
{
    size_t length = 0;

    while (length < in_size && in[length] != '\0')
        length++;
    if (length >= out_size)
        length = out_size - 1;
    memcpy(out, in, length);
    out[length] = '\0';
}

static void print_errno(const char *operation, const char *path)
{
    fprintf(stderr, "AUDIO_PROBE_ERROR operation=%s path=%s errno=%d (%s)\n",
            operation, path, errno, strerror(errno));
}

static int open_control(unsigned int card)
{
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "/dev/snd/controlC%u", card);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        print_errno("open-control", path);
    return fd;
}

static int print_card(unsigned int card, int fd)
{
    struct snd_ctl_card_info info;
    char path[64];
    char id[sizeof(info.id) + 1];
    char driver[sizeof(info.driver) + 1];
    char name[sizeof(info.name) + 1];

    memset(&info, 0, sizeof(info));
    if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info) < 0) {
        snprintf(path, sizeof(path), "/dev/snd/controlC%u", card);
        print_errno("card-info", path);
        return -1;
    }
    field_text(id, sizeof(id), info.id, sizeof(info.id));
    field_text(driver, sizeof(driver), info.driver, sizeof(info.driver));
    field_text(name, sizeof(name), info.name, sizeof(info.name));
    printf("AUDIO_CARD card=%u id=%s driver=%s name=%s\n",
           card, id, driver, name);
    return 0;
}

static void print_pcm_info(const struct snd_pcm_info *info, unsigned int card,
                           unsigned int device, int stream)
{
    char id[sizeof(info->id) + 1];
    char name[sizeof(info->name) + 1];
    char subname[sizeof(info->subname) + 1];

    field_text(id, sizeof(id), info->id, sizeof(info->id));
    field_text(name, sizeof(name), info->name, sizeof(info->name));
    field_text(subname, sizeof(subname), info->subname, sizeof(info->subname));
    printf("AUDIO_PCM card=%u device=%u stream=%s id=%s name=%s subname=%s "
           "subdevices=%u available=%u\n",
           card, device, stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
           id, name, subname, info->subdevices_count, info->subdevices_avail);
}

static int list_pcms(unsigned int card, int control_fd)
{
    int device = -1;
    int count = 0;
    char path[64];

    while (ioctl(control_fd, SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE, &device) == 0 &&
           device >= 0) {
        int stream;

        for (stream = SNDRV_PCM_STREAM_PLAYBACK;
             stream <= SNDRV_PCM_STREAM_CAPTURE; stream++) {
            struct snd_pcm_info info;

            memset(&info, 0, sizeof(info));
            info.device = (unsigned int)device;
            info.stream = stream;
            info.subdevice = 0;
            if (ioctl(control_fd, SNDRV_CTL_IOCTL_PCM_INFO, &info) < 0)
                continue;
            print_pcm_info(&info, card, (unsigned int)device, stream);
            count++;
        }
    }
    if (errno != ENOENT && errno != 0) {
        snprintf(path, sizeof(path), "/dev/snd/controlC%u", card);
        print_errno("pcm-next-device", path);
        return -1;
    }
    printf("AUDIO_PCM_COUNT=%d\n", count);
    return count ? 0 : 1;
}

static void set_any(struct snd_pcm_hw_params *params)
{
    unsigned int i;

    memset(params, 0, sizeof(*params));
    for (i = 0; i < sizeof(params->masks) / sizeof(params->masks[0]); i++)
        memset(&params->masks[i], 0xff, sizeof(params->masks[i]));
    for (i = 0; i < sizeof(params->intervals) / sizeof(params->intervals[0]); i++) {
        params->intervals[i].min = 0;
        params->intervals[i].max = UINT_MAX;
    }
    params->rmask = UINT_MAX;
}

static int print_refined_hw(int pcm_fd, const char *path)
{
    struct snd_pcm_hw_params params;
    unsigned int access;
    unsigned int format;

    set_any(&params);
    if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0) {
        print_errno("hw-refine", path);
        return -1;
    }
    access = params.masks[SNDRV_PCM_HW_PARAM_ACCESS].bits[0];
    format = params.masks[SNDRV_PCM_HW_PARAM_FORMAT].bits[0];
    printf("AUDIO_HW path=%s access_rw_interleaved=%s format_s16_le=%s "
           "channels=%u..%u rate=%u..%u period=%u..%u buffer=%u..%u\n",
           path,
           (access & (1U << SNDRV_PCM_ACCESS_RW_INTERLEAVED)) ? "yes" : "no",
           (format & (1U << SNDRV_PCM_FORMAT_S16_LE)) ? "yes" : "no",
           params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS -
                            SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
           params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS -
                            SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max,
           params.intervals[SNDRV_PCM_HW_PARAM_RATE -
                            SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
           params.intervals[SNDRV_PCM_HW_PARAM_RATE -
                            SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max,
           params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE -
                            SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
           params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE -
                            SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max,
           params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE -
                            SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
           params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE -
                            SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max);
    return 0;
}

static int probe_pcm(unsigned int card, unsigned int device, int stream)
{
    struct snd_pcm_info info;
    char path[64];
    int flags;
    int fd;

    snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%u%c", card, device,
             stream == SNDRV_PCM_STREAM_PLAYBACK ? 'p' : 'c');
    flags = stream == SNDRV_PCM_STREAM_PLAYBACK ? O_WRONLY : O_RDONLY;
    fd = open(path, flags | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        print_errno("open-pcm", path);
        return 1;
    }
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, SNDRV_PCM_IOCTL_INFO, &info) < 0) {
        print_errno("pcm-info", path);
        close(fd);
        return 1;
    }
    printf("AUDIO_OPEN path=%s result=success\n", path);
    print_pcm_info(&info, card, device, stream);
    if (print_refined_hw(fd, path) < 0) {
        close(fd);
        return 1;
    }
    close(fd);
    printf("AUDIO_PROBE_RESULT=PASS\n");
    return 0;
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

static int parse_stream(const char *text, int *stream)
{
    if (!strcmp(text, "playback")) {
        *stream = SNDRV_PCM_STREAM_PLAYBACK;
        return 0;
    }
    if (!strcmp(text, "capture")) {
        *stream = SNDRV_PCM_STREAM_CAPTURE;
        return 0;
    }
    return -1;
}

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s list | %s probe CARD DEVICE playback|capture\n",
            name, name);
}

int main(int argc, char **argv)
{
    unsigned int card = 0;
    unsigned int device = 0;
    int stream;
    int fd;
    int rc;

    if (argc == 2 && !strcmp(argv[1], "list")) {
        fd = open_control(card);
        if (fd < 0)
            return 1;
        rc = print_card(card, fd);
        if (rc == 0)
            rc = list_pcms(card, fd);
        close(fd);
        return rc == 0 ? 0 : 1;
    }
    if (argc == 5 && !strcmp(argv[1], "probe") &&
        parse_uint(argv[2], &card) == 0 &&
        parse_uint(argv[3], &device) == 0 &&
        parse_stream(argv[4], &stream) == 0)
        return probe_pcm(card, device, stream);

    usage(argv[0]);
    return 2;
}
