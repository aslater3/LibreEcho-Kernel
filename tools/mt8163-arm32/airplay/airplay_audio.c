/* LibreEcho AirPlay PCM bridge.
 *
 * Shairport Sync's pipe backend produces decoded interleaved S16_LE PCM.  The
 * MT8163 Echo kernel exposes a TinyALSA-compatible PCM device, while its old
 * ALSA ioctl implementation is not usable by libasound.  This deliberately
 * small bridge keeps the protocol daemon independent from that legacy driver
 * and writes the same device used by the validated tinyplay utility.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include <tinyalsa/mixer.h>
#include <tinyalsa/pcm.h>

#define DEFAULT_FIFO "/run/libreecho/airplay.pcm"
#define DEFAULT_CARD 0U
#define DEFAULT_DEVICE 23U
#define DEFAULT_RATE 48000U
#define DEFAULT_CHANNELS 2U
#define PERIOD_SIZE 1024U
#define PERIOD_COUNT 2U
#define DEFAULT_MIXER_VOLUME 127
#define AMP_SETTLE_US 30000U

static volatile sig_atomic_t stopping;

static void on_signal(int signo)
{
    if (signo == SIGTERM || signo == SIGINT)
        stopping = 1;
}

static int set_enum_control(struct mixer *mixer, const char *name,
                            const char *value)
{
    struct mixer_ctl *control = mixer_get_ctl_by_name(mixer, name);

    if (!control)
        return -1;
    return mixer_ctl_set_enum_by_string(control, value);
}

static int set_stereo_control(struct mixer *mixer, const char *name, int value)
{
    struct mixer_ctl *control = mixer_get_ctl_by_name(mixer, name);

    if (!control || mixer_ctl_get_num_values(control) < 2)
        return -1;
    if (mixer_ctl_set_value(control, 0, value) < 0)
        return -1;
    return mixer_ctl_set_value(control, 1, value);
}

static int set_pcm_volume(unsigned int card, int volume)
{
    struct mixer *mixer;
    int result = 0;

    if (volume < 0)
        return 0;
    mixer = mixer_open(card);
    if (!mixer) {
        fprintf(stderr, "airplay-audio: mixer %u unavailable\n", card);
        return -1;
    }
    if (set_stereo_control(mixer, "PCM Playback Volume", volume) < 0)
        result = -1;
    mixer_close(mixer);
    if (result < 0)
        fprintf(stderr, "airplay-audio: PCM volume control unavailable\n");
    return result;
}

/* Arm the output while keeping the codec muted.  This is used before a
 * stream exists, so enabling the physical amplifier cannot produce a pop. */
static int arm_output_controls(unsigned int card, int volume)
{
    struct mixer *mixer;
    int result = 0;

    mixer = mixer_open(card);
    if (!mixer) {
        fprintf(stderr, "airplay-audio: mixer %u unavailable\n", card);
        return -1;
    }
    if (set_enum_control(mixer, "MFP Gpio Mute", "On") < 0)
        result = -1;
    if (set_enum_control(mixer, "Ext_Speaker_Amp_Switch", "Off") < 0)
        result = -1;
    if (set_stereo_control(mixer, "HP DAC Playback Switch", 1) < 0)
        result = -1;
    if (volume >= 0 &&
        set_stereo_control(mixer, "PCM Playback Volume", volume) < 0)
        result = -1;
    mixer_close(mixer);
    return result;
}

/* Enable the physical amplifier only while muted, let its power rail settle,
 * then release the codec mute.  The old order (codec unmute, then amp on)
 * was the source of the audible cyclic scratch/pop on the tweeter path. */
static int enable_output_controls(unsigned int card)
{
    struct mixer *mixer;
    int result = 0;

    mixer = mixer_open(card);
    if (!mixer) {
        fprintf(stderr, "airplay-audio: mixer %u unavailable\n", card);
        return -1;
    }
    if (set_enum_control(mixer, "MFP Gpio Mute", "On") < 0)
        result = -1;
    if (set_enum_control(mixer, "Ext_Speaker_Amp_Switch", "On") < 0)
        result = -1;
    mixer_close(mixer);
    usleep(AMP_SETTLE_US);

    mixer = mixer_open(card);
    if (!mixer)
        return -1;
    if (set_enum_control(mixer, "MFP Gpio Mute", "Off") < 0)
        result = -1;
    mixer_close(mixer);
    if (result < 0)
        fprintf(stderr, "airplay-audio: output enable controls unavailable\n");
    return result;
}

static int disable_output_controls(unsigned int card)
{
    struct mixer *mixer;
    int result = 0;

    mixer = mixer_open(card);
    if (!mixer) {
        fprintf(stderr, "airplay-audio: mixer %u unavailable\n", card);
        return -1;
    }
    /* Mute before removing amplifier power to avoid a shutdown pop. */
    if (set_enum_control(mixer, "MFP Gpio Mute", "On") < 0)
        result = -1;
    if (set_enum_control(mixer, "Ext_Speaker_Amp_Switch", "Off") < 0)
        result = -1;
    mixer_close(mixer);
    if (result < 0)
        fprintf(stderr, "airplay-audio: output disable controls unavailable\n");
    return result;
}

static int airplay_volume_to_mixer(double db)
{
    double fraction;
    int value;

    if (db <= -143.0)
        return 0;
    if (db < -30.0)
        db = -30.0;
    if (db > 0.0)
        db = 0.0;
    fraction = (db + 30.0) / 30.0;
    value = (int)lround(fraction * 175.0);
    if (value < 0)
        return 0;
    if (value > 175)
        return 175;
    return value;
}

/* Read one hardware period without allowing a FIFO/network pause to starve
 * the two-period MT8163 DMA buffer.  Once playback has started, an incomplete
 * period is padded with silence; this is preferable to an ALSA XRUN and keeps
 * the output clock running through short AirPlay delivery gaps. */
static int read_period(int fd, void *buffer, size_t length, int fill_silence,
                       int *ended)
{
    unsigned char *cursor = buffer;
    size_t received = 0;

    *ended = 0;

    while (received < length && !stopping) {
        ssize_t count = read(fd, cursor + received, length - received);
        if (count > 0) {
            received += (size_t)count;
            continue;
        }
        if (count == 0) {
            *ended = 1;
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pollfd = {
                .fd = fd,
                .events = POLLIN,
            };
            int poll_result = poll(&pollfd, 1, 20);
            if (poll_result < 0 && errno != EINTR) {
                fprintf(stderr, "airplay-audio: FIFO poll failed: %s\n",
                        strerror(errno));
                return -1;
            }
            if (poll_result == 0) {
                if (received > 0)
                    break;
                if (fill_silence) {
                    memset(buffer, 0, length);
                    return 1;
                }
            }
            continue;
        }
        fprintf(stderr, "airplay-audio: FIFO read failed: %s\n", strerror(errno));
        return -1;
    }
    if (stopping)
        return -1;
    if (received == 0)
        return 0;
    if (received < length)
        memset(cursor + received, 0, length - received);
    return 1;
}

static int ensure_fifo(const char *path)
{
    struct stat st;

    if (mkfifo(path, 0666) == 0)
        return 0;
    if (errno != EEXIST)
        return -1;
    if (stat(path, &st) < 0 || !S_ISFIFO(st.st_mode)) {
        errno = EEXIST;
        return -1;
    }
    return 0;
}

static int play_stream(const char *fifo, unsigned int card, unsigned int device,
                       unsigned int rate, unsigned int channels)
{
    struct pcm_config config = {
        .channels = channels,
        .rate = rate,
        .period_size = PERIOD_SIZE,
        .period_count = PERIOD_COUNT,
        .format = PCM_FORMAT_S16_LE,
        /* Start DMA as soon as the first complete period is accepted.  The
         * amplifier is enabled only after that write, so waiting for a full
         * period threshold can leave the codec/amp sequencing ahead of the
         * actual stream and produce the observed tweeter click. */
        .start_threshold = 1U,
        .stop_threshold = PERIOD_SIZE * PERIOD_COUNT,
        .silence_threshold = PERIOD_SIZE * PERIOD_COUNT,
        .silence_size = 0,
        .avail_min = 0,
    };
    const size_t frame_bytes = channels * sizeof(int16_t);
    const size_t buffer_bytes = PERIOD_SIZE * frame_bytes;
    unsigned char *buffer;
    struct pcm *pcm = NULL;
    int fifo_fd = -1;
    int result = -1;

    if (ensure_fifo(fifo) < 0) {
        fprintf(stderr, "airplay-audio: cannot create FIFO %s: %s\n",
                fifo, strerror(errno));
        return -1;
    }
    fifo_fd = open(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0) {
        if (!stopping)
            fprintf(stderr, "airplay-audio: cannot open FIFO %s: %s\n",
                    fifo, strerror(errno));
        return -1;
    }

    buffer = malloc(buffer_bytes);
    if (!buffer) {
        fprintf(stderr, "airplay-audio: PCM buffer allocation failed\n");
        goto out;
    }
    for (;;) {
        int ended;
        int read_result = read_period(fifo_fd, buffer, buffer_bytes, 0, &ended);

        if (read_result < 0)
            goto out;
        if (read_result > 0)
            break;
        if (stopping)
            goto out;
        usleep(250000);
    }

    pcm = pcm_open(card, device, PCM_OUT, &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        fprintf(stderr, "airplay-audio: PCM %u,%u unavailable: %s\n",
                card, device, pcm ? pcm_get_error(pcm) : "open failed");
        goto out;
    }
    if (pcm_prepare(pcm) < 0) {
        fprintf(stderr, "airplay-audio: PCM prepare failed: %s\n",
                pcm_get_error(pcm));
        goto out;
    }
    fprintf(stderr, "airplay-audio: streaming S16_LE %u Hz, %u channels to PCM %u,%u\n",
            rate, channels, card, device);

    if (pcm_writei(pcm, buffer, PERIOD_SIZE) != (int)PERIOD_SIZE) {
        fprintf(stderr, "airplay-audio: PCM write failed: %s\n",
                pcm_get_error(pcm));
        goto out;
    }

    /* The Amazon codec applies its playback mute during prepare/start.  Do
     * this after the first write has started the DMA stream, otherwise the
     * codec immediately puts the physical amplifier back into mute and the
     * output has a clipped/crackling transition. */
    if (enable_output_controls(card) < 0)
        fprintf(stderr, "airplay-audio: playback mixer enable incomplete\n");

    while (!stopping) {
        int ended;
        int read_result = read_period(fifo_fd, buffer, buffer_bytes, 1, &ended);
        if (read_result < 0)
            goto out;
        if (read_result == 0)
            break;
        if (pcm_writei(pcm, buffer, PERIOD_SIZE) != (int)PERIOD_SIZE) {
            fprintf(stderr, "airplay-audio: PCM write failed: %s\n",
                    pcm_get_error(pcm));
            goto out;
        }
        if (ended)
            break;
    }
    result = 0;

out:
    if (pcm)
        pcm_close(pcm);
    free(buffer);
    close(fifo_fd);
    return result;
}

int main(int argc, char **argv)
{
    const char *fifo = DEFAULT_FIFO;
    unsigned int card = DEFAULT_CARD;
    unsigned int device = DEFAULT_DEVICE;
    unsigned int rate = DEFAULT_RATE;
    unsigned int channels = DEFAULT_CHANNELS;
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (argc == 2 && !strcmp(argv[1], "--start"))
        return arm_output_controls(card, DEFAULT_MIXER_VOLUME) < 0;
    if (argc == 2 && !strcmp(argv[1], "--stop"))
        return disable_output_controls(card) < 0;
    if (argc == 3 && !strcmp(argv[1], "--set-volume")) {
        char *end;
        double db = strtod(argv[2], &end);

        if (end == argv[2] || *end != '\0')
            return 2;
        if (set_pcm_volume(card, airplay_volume_to_mixer(db)) < 0)
            return 1;
        return 0;
    }

    if (argc > 1) fifo = argv[1];
    if (argc > 2) card = (unsigned int)strtoul(argv[2], NULL, 10);
    if (argc > 3) device = (unsigned int)strtoul(argv[3], NULL, 10);
    if (argc > 4) rate = (unsigned int)strtoul(argv[4], NULL, 10);
    if (argc > 5) channels = (unsigned int)strtoul(argv[5], NULL, 10);
    if (argc > 6 || channels == 0 || channels > 2 || rate == 0) {
        fprintf(stderr, "Usage: %s [fifo] [card] [device] [rate] [channels]\n", argv[0]);
        return 2;
    }

    while (!stopping) {
        if (play_stream(fifo, card, device, rate, channels) < 0 && !stopping)
            usleep(250000);
    }
    return 0;
}
