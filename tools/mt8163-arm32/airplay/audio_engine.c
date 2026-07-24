/* LibreEcho shared PCM engine.
 *
 * This is the sole owner of the MT8163 playback PCM and board amplifier.  All
 * producers write S16_LE/48 kHz/stereo to one of four named buses.  The engine
 * mixes those buses into the mono programme feed expected by Puffin's
 * calibrated tweeter/woofer codec profile, ducks media under higher-priority
 * audio, applies the stock +3 dB trim with linked limiting, and performs the
 * validated mute/amplifier sequence exactly once.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <tinyalsa/mixer.h>
#include <tinyalsa/pcm.h>

#include "puffin_downmix.h"

#define DEFAULT_ROOT "/run/libreecho-audio"
#define LED_SOCKET "/run/libreecho/led.sock"
#define DEFAULT_CARD 0U
#define DEFAULT_DEVICE 23U
#define DEFAULT_RATE 48000U
#define DEFAULT_CHANNELS 2U
#define PERIOD_SIZE 2048U
#define PERIOD_COUNT 2U
#define AMP_SETTLE_US 30000U
#define SOURCE_COUNT 4U
#define SOURCE_IDLE_PERIODS 8U
#define MEDIA_DUCK_Q15 8231

enum source_role {
	SOURCE_MEDIA,
	SOURCE_SYSTEM,
	SOURCE_ANNOUNCEMENT,
	SOURCE_ALARM
};

struct source_bus {
	const char *name;
	enum source_role role;
	char path[256];
	int fd;
	unsigned int idle_periods;
	int32_t gain_q15;
	int16_t *samples;
	size_t received;
};

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

static int set_single_control(struct mixer *mixer, const char *name, int value)
{
    struct mixer_ctl *control = mixer_get_ctl_by_name(mixer, name);

    if (!control || mixer_ctl_get_num_values(control) < 1)
        return -1;
    return mixer_ctl_set_value(control, 0, value);
}

/* Arm the output while keeping the codec muted.  This is used before a
 * stream exists, so enabling the physical amplifier cannot produce a pop. */
static int arm_output_controls(unsigned int card, int volume)
{
    struct mixer *mixer;
    int result = 0;

    mixer = mixer_open(card);
    if (!mixer) {
        fprintf(stderr, "audio-engine: mixer %u unavailable\n", card);
        return -1;
    }
    if (set_enum_control(mixer, "MFP Gpio Mute", "On") < 0)
        result = -1;
    if (set_enum_control(mixer, "Ext_Speaker_Amp_Switch", "Off") < 0)
        result = -1;
    if (set_enum_control(mixer, "Audio_DacMux_Setting", "Off") < 0)
        result = -1;
    if (set_enum_control(mixer, "Right Channel Only", "Off") < 0)
        result = -1;
    if (set_stereo_control(mixer, "HP DAC Playback Switch", 1) < 0)
        result = -1;
    if (set_single_control(mixer, "HPL Output Mixer L_DAC Switch", 1) < 0)
        result = -1;
    if (set_single_control(mixer, "HPR Output Mixer R_DAC Switch", 1) < 0)
        result = -1;
    if (set_single_control(mixer, "HPR Output Mixer IN1_R Switch", 0) < 0)
        result = -1;
    if (set_stereo_control(mixer, "HP Driver Gain Volume", 6) < 0)
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
        fprintf(stderr, "audio-engine: mixer %u unavailable\n", card);
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
        fprintf(stderr, "audio-engine: output enable controls unavailable\n");
    return result;
}

static int disable_output_controls(unsigned int card)
{
    struct mixer *mixer;
    int result = 0;

    mixer = mixer_open(card);
    if (!mixer) {
        fprintf(stderr, "audio-engine: mixer %u unavailable\n", card);
        return -1;
    }
    /* Mute before removing amplifier power to avoid a shutdown pop. */
    if (set_enum_control(mixer, "MFP Gpio Mute", "On") < 0)
        result = -1;
    if (set_enum_control(mixer, "Ext_Speaker_Amp_Switch", "Off") < 0)
        result = -1;
    mixer_close(mixer);
    if (result < 0)
        fprintf(stderr, "audio-engine: output disable controls unavailable\n");
    return result;
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

/* LED indication is deliberately best-effort: an unavailable or wedged LED
 * daemon must never block or interrupt an announcement.  The owner field lets
 * ledd restore a pairing or user pattern after this temporary override. */
static void set_announcement_led(int active)
{
	struct sockaddr_un address;
	struct pollfd pollfd;
	const char *request_on =
		"{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
		"{\"name\":\"pulse\",\"r\":0,\"g\":255,\"b\":0,"
		"\"brightness\":55,\"repeats\":0,"
		"\"owner\":\"announcement\"}}\n";
	const char *request_off =
		"{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
		"{\"name\":\"stop\",\"owner\":\"announcement\"}}\n";
	const char *request = active ? request_on : request_off;
	size_t length = strlen(request);
	socklen_t error_length;
	int socket_error = 0;
	int fd;
	int result;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (strlen(LED_SOCKET) >= sizeof(address.sun_path)) {
		close(fd);
		return;
	}
	strcpy(address.sun_path, LED_SOCKET);
	result = connect(fd, (struct sockaddr *)&address, sizeof(address));
	if (result < 0 && errno != EINPROGRESS && errno != EAGAIN) {
		close(fd);
		return;
	}
	if (result < 0) {
		pollfd.fd = fd;
		pollfd.events = POLLOUT;
		pollfd.revents = 0;
		do {
			result = poll(&pollfd, 1, 20);
		} while (result < 0 && errno == EINTR);
		error_length = sizeof(socket_error);
		if (result <= 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
			       &error_length) < 0 ||
		    socket_error != 0) {
			close(fd);
			return;
		}
	}
	(void)send(fd, request, length, MSG_DONTWAIT | MSG_NOSIGNAL);
	close(fd);
}

static void sync_announcement_led(const struct source_bus *sources,
				  int *active)
{
	int wanted = sources[SOURCE_ANNOUNCEMENT].idle_periods > 0;

	if (wanted == *active)
		return;
	set_announcement_led(wanted);
	*active = wanted;
}

static void clear_source_activity(struct source_bus *sources,
				  int *announcement_led_active)
{
	unsigned int i;

	for (i = 0; i < SOURCE_COUNT; ++i)
		sources[i].idle_periods = 0;
	sync_announcement_led(sources, announcement_led_active);
}

static int32_t db_to_q15(double db)
{
	double gain;

	if (db <= -144.0)
		return 0;
	if (db > 0.0)
		db = 0.0;
	gain = pow(10.0, db / 20.0) * 32768.0;
	if (gain <= 0.0)
		return 0;
	if (gain >= 32768.0)
		return 32768;
	return (int32_t)lround(gain);
}

static int read_media_gain(const char *root)
{
	char path[256];
	char buffer[64];
	char *end;
	double db;
	int fd;
	ssize_t n;

	if (snprintf(path, sizeof(path), "%s/media.volume", root) < 0)
		return 32768;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 32768;
	n = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (n <= 0)
		return 32768;
	buffer[n] = '\0';
	db = strtod(buffer, &end);
	if (end == buffer || !isfinite(db))
		return 32768;
	return db_to_q15(db);
}

static int setup_sources(struct source_bus *sources, const char *root)
{
	static const char *const names[SOURCE_COUNT] = {
		"media", "system", "announcement", "alarm"
	};
	unsigned int i;
	const size_t bytes = PERIOD_SIZE * DEFAULT_CHANNELS * sizeof(int16_t);

	if (mkdir(root, 0770) < 0 && errno != EEXIST)
		return -1;
	for (i = 0; i < SOURCE_COUNT; ++i) {
		memset(&sources[i], 0, sizeof(sources[i]));
		sources[i].name = names[i];
		sources[i].role = (enum source_role)i;
		sources[i].fd = -1;
		sources[i].gain_q15 = 32768;
		if (snprintf(sources[i].path, sizeof(sources[i].path),
			     "%s/%s.pcm", root, names[i]) < 0 ||
		    ensure_fifo(sources[i].path) < 0)
			return -1;
		sources[i].fd = open(sources[i].path,
				     O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (sources[i].fd < 0)
			return -1;
		sources[i].samples = calloc(1, bytes);
		if (!sources[i].samples)
			return -1;
	}
	return 0;
}

static void close_sources(struct source_bus *sources)
{
	unsigned int i;

	for (i = 0; i < SOURCE_COUNT; ++i) {
		if (sources[i].fd >= 0)
			close(sources[i].fd);
		free(sources[i].samples);
		sources[i].samples = NULL;
		sources[i].fd = -1;
	}
}

static int poll_sources(struct source_bus *sources, int timeout_ms)
{
	struct pollfd pollfds[SOURCE_COUNT];
	unsigned int i;
	int result;

	for (i = 0; i < SOURCE_COUNT; ++i) {
		pollfds[i].fd = sources[i].fd;
		pollfds[i].events = POLLIN;
		pollfds[i].revents = 0;
	}
	do {
		result = poll(pollfds, SOURCE_COUNT, timeout_ms);
	} while (result < 0 && errno == EINTR && !stopping);
	return result;
}

static int read_sources(struct source_bus *sources, const char *root)
{
	const size_t bytes = PERIOD_SIZE * DEFAULT_CHANNELS * sizeof(int16_t);
	unsigned int i;
	int received_any = 0;

	for (i = 0; i < SOURCE_COUNT; ++i) {
		unsigned char *cursor = (unsigned char *)sources[i].samples;

		sources[i].received = 0;
		memset(cursor, 0, bytes);
		while (sources[i].received < bytes) {
			ssize_t n = read(sources[i].fd,
					 cursor + sources[i].received,
					 bytes - sources[i].received);

			if (n > 0) {
				sources[i].received += (size_t)n;
				received_any = 1;
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (n == 0)
				break;
			return -1;
		}
		if (sources[i].received > 0)
			sources[i].idle_periods = SOURCE_IDLE_PERIODS;
		else if (sources[i].idle_periods > 0)
			--sources[i].idle_periods;
	}
	sources[SOURCE_MEDIA].gain_q15 = read_media_gain(root);
	return received_any;
}

static int sources_active(const struct source_bus *sources)
{
	unsigned int i;

	for (i = 0; i < SOURCE_COUNT; ++i)
		if (sources[i].idle_periods > 0)
			return 1;
	return 0;
}

static void render_period(struct source_bus *sources, int16_t *output,
			  struct puffin_dynamics *dynamics)
{
	size_t frame;
	int higher_priority =
		sources[SOURCE_SYSTEM].received > 0 ||
		sources[SOURCE_ANNOUNCEMENT].received > 0 ||
		sources[SOURCE_ALARM].received > 0;
	int alarm_active = sources[SOURCE_ALARM].received > 0;

	for (frame = 0; frame < PERIOD_SIZE; ++frame) {
		int32_t mixed = 0;
		unsigned int source;

		for (source = 0; source < SOURCE_COUNT; ++source) {
			size_t available_frames = sources[source].received /
				(DEFAULT_CHANNELS * sizeof(int16_t));
			int32_t mono;
			int32_t gain;

			if (frame >= available_frames)
				continue;
			mono = (int32_t)sources[source].samples[frame * 2] +
			       (int32_t)sources[source].samples[frame * 2 + 1];
			mono /= 2;
			gain = sources[source].gain_q15;
			if (source == SOURCE_MEDIA && alarm_active)
				gain = 0;
			else if (source == SOURCE_MEDIA && higher_priority)
				gain = (gain * MEDIA_DUCK_Q15) >> 15;
			mixed += (int32_t)(((int64_t)mono * gain) >> 15);
		}
		output[frame * 2] = puffin_render_mono(dynamics, mixed);
		output[frame * 2 + 1] = output[frame * 2];
	}
}

static int run_engine(const char *root, unsigned int card, unsigned int device)
{
	struct pcm_config config = {
		.channels = DEFAULT_CHANNELS,
		.rate = DEFAULT_RATE,
		.period_size = PERIOD_SIZE,
		.period_count = PERIOD_COUNT,
		.format = PCM_FORMAT_S16_LE,
		.start_threshold = 1U,
		.stop_threshold = PERIOD_SIZE * PERIOD_COUNT,
		.silence_threshold = PERIOD_SIZE * PERIOD_COUNT,
		.silence_size = 0,
		.avail_min = 0,
	};
	const size_t bytes = PERIOD_SIZE * DEFAULT_CHANNELS * sizeof(int16_t);
	struct source_bus sources[SOURCE_COUNT];
	struct puffin_dynamics dynamics;
	int16_t *output = NULL;
	int16_t *second = NULL;
	int result = -1;
	int announcement_led_active = 0;
	unsigned int i;

	memset(sources, 0, sizeof(sources));
	for (i = 0; i < SOURCE_COUNT; ++i)
		sources[i].fd = -1;
	if (setup_sources(sources, root) < 0) {
		fprintf(stderr, "audio-engine: source setup failed: %s\n",
			strerror(errno));
		goto out;
	}
	output = malloc(bytes * 2);
	if (!output)
		goto out;
	second = output + PERIOD_SIZE * DEFAULT_CHANNELS;
	fprintf(stderr,
		"audio-engine: ready (root=%s, S16_LE/48000/stereo, PCM %u,%u)\n",
		root, card, device);

	while (!stopping) {
		struct pcm *pcm = NULL;

		if (poll_sources(sources, -1) < 0)
			break;
		if (stopping)
			break;
		if (read_sources(sources, root) <= 0)
			continue;
		sync_announcement_led(sources, &announcement_led_active);
		puffin_dynamics_init(&dynamics);
		render_period(sources, output, &dynamics);
		(void)poll_sources(sources, 20);
		if (read_sources(sources, root) < 0)
			break;
		sync_announcement_led(sources, &announcement_led_active);
		render_period(sources, second, &dynamics);

		if (arm_output_controls(card, -1) < 0) {
			fprintf(stderr, "audio-engine: output arm failed\n");
			clear_source_activity(sources, &announcement_led_active);
			continue;
		}
		pcm = pcm_open(card, device, PCM_OUT, &config);
		if (!pcm || !pcm_is_ready(pcm)) {
			fprintf(stderr, "audio-engine: PCM %u,%u unavailable: %s\n",
				card, device,
				pcm ? pcm_get_error(pcm) : "open failed");
			if (pcm)
				pcm_close(pcm);
			(void)disable_output_controls(card);
			clear_source_activity(sources, &announcement_led_active);
			usleep(250000);
			continue;
		}
		if (pcm_prepare(pcm) < 0 ||
		    pcm_writei(pcm, output, PERIOD_SIZE) != (int)PERIOD_SIZE ||
		    pcm_writei(pcm, second, PERIOD_SIZE) != (int)PERIOD_SIZE ||
		    enable_output_controls(card) < 0) {
			fprintf(stderr, "audio-engine: playback start failed: %s\n",
				pcm_get_error(pcm));
			(void)disable_output_controls(card);
			pcm_close(pcm);
			clear_source_activity(sources, &announcement_led_active);
			continue;
		}

		while (!stopping && sources_active(sources)) {
			(void)poll_sources(sources, 20);
			if (read_sources(sources, root) < 0) {
				stopping = 1;
				break;
			}
			sync_announcement_led(sources, &announcement_led_active);
			render_period(sources, output, &dynamics);
			if (pcm_writei(pcm, output, PERIOD_SIZE) !=
			    (int)PERIOD_SIZE) {
				fprintf(stderr,
					"audio-engine: PCM write failed: %s\n",
					pcm_get_error(pcm));
				break;
			}
		}
		clear_source_activity(sources, &announcement_led_active);
		(void)disable_output_controls(card);
		pcm_close(pcm);
	}
	result = stopping ? 0 : -1;
out:
	if (announcement_led_active)
		set_announcement_led(0);
	free(output);
	close_sources(sources);
	return result;
}

int main(int argc, char **argv)
{
	const char *root = DEFAULT_ROOT;
	unsigned int card = DEFAULT_CARD;
	unsigned int device = DEFAULT_DEVICE;
	struct sigaction action;

	if (argc > 1)
		root = argv[1];
	if (argc > 2)
		card = (unsigned int)strtoul(argv[2], NULL, 10);
	if (argc > 3)
		device = (unsigned int)strtoul(argv[3], NULL, 10);
	if (argc > 4) {
		fprintf(stderr, "Usage: %s [bus-root] [card] [device]\n", argv[0]);
		return 2;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	(void)sigaction(SIGTERM, &action, NULL);
	(void)sigaction(SIGINT, &action, NULL);
	signal(SIGPIPE, SIG_IGN);
	return run_engine(root, card, device) < 0;
}
