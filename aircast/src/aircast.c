/*
 *  AirCast: Chromecast to AirPlay
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 * See LICENSE
 *
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <locale.h>
#ifdef _WIN32
#include <process.h>
#endif

#include "cross_net.h"
#include "cross_util.h"
#include "cross_thread.h"
#include "cross_log.h"
#include "cross_ssl.h"

#include "aircast.h"
#include "metadata.h"
#include "cast_util.h"
#include "cast_parse.h"
#include "castitf.h"
#include "mdnssd.h"
#include "mdnssvc.h"
#include "config_cast.h"
#include "ixml.h"

#define DISCOVERY_TIME 	20
#define MEDIA_VOLUME	0.5

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/
struct sMR	*glMRDevices;
uint16_t	glPortBase, glPortRange, glPicoPort;
int32_t		glLogLimit = -1;
int			glMaxDevices = 32;
char		glBinding[16] = "?";

log_level	main_loglevel = lINFO;
log_level	raop_loglevel = lINFO;
log_level	util_loglevel = lWARN;
log_level	cast_loglevel = lINFO;

tMRConfig			glMRConfig = {
							true,	// enabled
							false,	// stop_receiver
							"",		// name
							"flac",	// use_flac
							true,	// metadata
							true,	// flush
							MEDIA_VOLUME,	// media volume (0..1)
							{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
							"",		// rtp/http_latency (0 = use client's request)
							false,	// drift
							"", 	// artwork
					};



/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level*			loglevel = &main_loglevel;
#if LINUX || FREEBSD || SUNOS
static bool					glDaemonize = false;
#endif
static bool					glMainRunning = true;
static struct mdnssd_handle_s* glmDNSsearchHandle;
static struct in_addr 		glHost;
static pthread_t 			glMainThread, glmDNSsearchThread;
static char*				glLogFile;
static bool					glDiscovery = false;
static bool					glInteractive = true;
static char*				glPidFile = NULL;
static bool					glAutoSaveConfigFile = false;
static bool					glGracefullShutdown = true;
static void*				glConfigID = NULL;
static char					glConfigName[STR_LEN] = "./config.xml";
static struct mdnsd*		glmDNSServer = NULL;
static pthread_mutex_t		glMainMutex;
static uint32_t				glNetmask;
static char*				glNameFormat = "%s+";
static uint16_t			glMetricsPort;
static bool				glMetricsRunning;
static int				glMetricsSock = -1;
static pthread_t			glMetricsThread;
static bool				glMusicAssistantProfile;

static void ApplyMusicAssistantProfile(tMRConfig *config) {
	config->SoftFlush = config->GroupOptimized = config->PersistentStream = config->RecoveryEnabled = true;
	config->SoftFlushTimeoutMs = 1500; config->ReaderStallMs = 1200; config->PlayRetryMs = 700;
	config->MaxRetries = 3; config->PrebufferMs = 150; config->PlayDedupeMs = 250;
	config->GroupStartupGraceMs = 1500;
}

static char usage[] =
			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -b <ip|iface>network  address or interface to bind to\n"
		   "  -a <port>[:<count>]   set inbound port and range for RTP and HTTP\n"
		   "  -c <mp3[:<rate>]|aac[:<rate>]|flac[:0..9][/1152...16384]|wav>\taudio format send to player\n"
   		   "  -v <0..1>             group MediaVolume factor\n"
		   "  -x <config file>      read config from file (default is ./config.xml)\n"
		   "  -i <config file>      discover players, save <config file> and exit\n"
		   "  -I                    auto save config at every network scan\n"
		   "  -N <format>           transform device name using C format (%s=name)\n"
		   "  -l <[rtp][:http][:f]> RTP and HTTP latency (ms), ':f' forces silence fill\n"
		   "  -r                    let timing reference drift (no click)\n"
		   "  -f <logfile>          write debug to logfile\n"
		   "  -p <pid file>         write PID in file\n"
		   "  -d <log>=<level>      set logging level, logs: all|raop|main|util|cast, level: error|warn|info|debug|sdebug\n"
#if LINUX || FREEBSD
		   "  -z                    daemonize\n"
#endif
		   "  -Z                    NOT interactive\n"
		   "  -k                    immediate exit on SIGQUIT and SIGTERM\n"
		   "  -t                    license terms\n"
		   "  --noflush             ignore flush command (wait for teardown to stop)\n"
		   "  --profile music-assistant  enable Cast Group recovery profile\n"
		   "  --metrics-port <port>  expose Prometheus metrics on /metrics\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
		   "\n\n";

static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n"
	;


/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void *MRThread(void *args);
static bool  AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool Group, struct in_addr ip, uint16_t port);
static void  RemoveCastDevice(struct sMR *Device);
static bool	 Start(bool cold);
static bool	 Stop(bool exit);

static void *MetricsThread(void *args) {
	(void) args;
	while (glMetricsRunning) {
		fd_set fds;
		struct timeval timeout = { 0, 250 * 1000 };
		FD_ZERO(&fds); FD_SET(glMetricsSock, &fds);
		if (select(glMetricsSock + 1, &fds, NULL, NULL, &timeout) <= 0 || !FD_ISSET(glMetricsSock, &fds)) continue;
		int sock = accept(glMetricsSock, NULL, NULL);
		if (sock < 0) continue;
		char request[256] = "", body[65536];
		struct timeval clientTimeout = { 1, 0 };
		FD_ZERO(&fds); FD_SET(sock, &fds);
		if (select(sock + 1, &fds, NULL, NULL, &clientTimeout) <= 0) {
			closesocket(sock);
			continue;
		}
		recv(sock, request, sizeof(request) - 1, 0);
		if (strncmp(request, "GET /metrics ", 13)) {
			send(sock, "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n", 45, 0);
			closesocket(sock);
			continue;
		}
		size_t used = 0;
		for (int i = 0; i < glMaxDevices && used < sizeof(body); i++) {
			struct sMR *p = &glMRDevices[i];
			pthread_mutex_lock(&p->Mutex);
			if (p->Running) {
				raopsr_stats_t stats = { 0 };
				raopsr_get_stats(p->Raop, &stats);
				int written = snprintf(body + used, sizeof(body) - used,
				"airconnect_cast_startup_latency_ms{device=\"%s\"} %u\n"
				"airconnect_cast_resume_latency_ms{device=\"%s\"} %u\n"
				"airconnect_cast_reader_stalls_total{device=\"%s\"} %u\n"
				"airconnect_cast_retries_total{device=\"%s\"} %u\n"
				"airconnect_cast_reloads_total{device=\"%s\"} %u\n"
				"airconnect_buffer_fill_frames{device=\"%s\"} %u\n"
				"airconnect_session_state{device=\"%s\"} %u\n",
				p->Config.Name, p->CastPlayingAt ? p->CastPlayingAt - p->SessionStarted : 0,
				p->Config.Name, p->ResumeLatencyMs, p->Config.Name, p->TotalReaderStalls,
				p->Config.Name, p->TotalRetries, p->Config.Name, p->TotalReloads, p->Config.Name, stats.fill,
				p->Config.Name, (unsigned) p->CastSession);
				used += written < 0 ? 0 : min((size_t) written, sizeof(body) - used - 1);
			}
			pthread_mutex_unlock(&p->Mutex);
		}
		char header[128];
		int headerLen = snprintf(header, sizeof(header), "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", used);
		send(sock, header, headerLen, 0);
		send(sock, body, used, 0);
		closesocket(sock);
	}
	return NULL;
}

static bool StartMetrics(void) {
	if (!glMetricsPort || glMetricsRunning) return true;
	glMetricsSock = bind_socket(glHost, &glMetricsPort, SOCK_STREAM);
	if (glMetricsSock < 0) return false;
	if (listen(glMetricsSock, 4)) {
		closesocket(glMetricsSock);
		glMetricsSock = -1;
		return false;
	}
	glMetricsRunning = true;
	pthread_create(&glMetricsThread, NULL, &MetricsThread, NULL);
	LOG_INFO("Metrics listening on port %hu", glMetricsPort);
	return true;
}

static void StopMetrics(void) {
	if (!glMetricsRunning) return;
	glMetricsRunning = false;
	shutdown_socket(glMetricsSock);
	pthread_join(glMetricsThread, NULL);
	glMetricsSock = -1;
}

static void LoadCurrentStream(struct sMR *device) {
	metadata_t metadata = { .title = "Streaming from AirConnect", .duration = 0, .track = 0 };
	char *contentType = "audio/flac";
	if (*device->Config.ArtWork) metadata.artwork = device->Config.ArtWork;
	if (strcasestr(device->Config.Codec, "mp3")) contentType = "audio/mpeg";
	else if (strcasestr(device->Config.Codec, "aac")) contentType = "audio/aac";
	else if (strcasestr(device->Config.Codec, "wav")) contentType = "audio/wav";
	CastSetGeneration(device->CastCtx, device->Generation);
	CastLoad(device->CastCtx, device->StreamUri, contentType, device->Name, &metadata, 0);
	CastPlayRetry(device->CastCtx);
}

static void StartRecovery(struct sMR *device, uint32_t now, const char *reason) {
	if (!device->Config.RecoveryEnabled || !*device->StreamUri ||
		now - device->LastRecoveryAt < device->Config.PlayRetryMs) return;
	device->LastRecoveryAt = now;
	device->ReaderStalls++;
	device->TotalReaderStalls++;
	if (device->RetryCount++ >= device->Config.MaxRetries) {
		device->CastSession = CAST_SESSION_FAILED;
		LOG_ERROR("[%p]: CAST recovery failed generation=%u retries=%u", device, device->Generation, device->RetryCount);
		raopsr_notify(device->Raop, RAOP_STOP, NULL);
		return;
	}
	device->CastSession = CAST_SESSION_RECOVERING;
	device->TotalRetries++;
	if (device->RetryCount == 1) {
		LOG_WARN("[%p]: %s, retrying PLAY generation=%u", device, reason, device->Generation);
		CastPlayRetry(device->CastCtx);
	} else if (device->RetryCount == 2) {
		LOG_WARN("[%p]: %s, reloading generation=%u", device, reason, device->Generation);
		device->ExpectStop = true;
		CastStop(device->CastCtx);
		device->Generation++; device->Reloads++; device->TotalReloads++;
		device->HttpGetAt = device->FirstReadAt = device->CastPlayingAt = 0;
		LoadCurrentStream(device);
	} else {
		LOG_WARN("[%p]: %s, reconnecting receiver generation=%u", device, reason, device->Generation);
		CastPowerOff(device->CastCtx);
		device->Generation++; device->Reloads++; device->TotalReloads++;
		device->HttpGetAt = device->FirstReadAt = device->CastPlayingAt = 0;
		LoadCurrentStream(device);
	}
}

static void raop_http_cb(void *owner, struct key_data_s *headers, struct key_data_s *response) {
	struct sMR *device = owner;
	uint32_t now = gettime_ms();
	(void) headers; (void) response;
	pthread_mutex_lock(&device->Mutex);
	if (device->Running && !device->HttpGetAt) {
		device->HttpGetAt = now;
		LOG_INFO("[%p]: HTTP GET +%ums generation=%u", device, now - device->SessionStarted, device->Generation);
	}
	pthread_mutex_unlock(&device->Mutex);
}

/*----------------------------------------------------------------------------*/
static void raop_cb(void *owner, raopsr_event_t event, ...) {
	struct sMR *Device = (struct sMR*) owner;
	va_list args;
	va_start(args, event);

	pthread_mutex_lock(&Device->Mutex);

	// this is async, so player might have been deleted
	if (!Device->Running) {
		LOG_WARN("[%p]: device has been removed", owner);
		pthread_mutex_unlock(&Device->Mutex);
		return;
	}

	switch (event) {
		case RAOP_STREAM:
			// a PLAY will come later, so we'll do the load at that time
			Device->Generation++; Device->RetryCount = 0;
			Device->SessionStarted = gettime_ms();
			Device->ResumeStartedAt = Device->ResumeLatencyMs = 0;
			Device->FirstRtpAt = Device->FirstSyncAt = Device->FirstAudioAt = Device->HttpGetAt = Device->FirstReadAt = Device->CastPlayingAt = 0;
			Device->StreamUri[0] = '\0'; Device->CastSession = CAST_SESSION_IDLE;
			Device->State = STOPPED; Device->ExpectStop = false; Device->SoftPausedAt = Device->LastRecoveryAt = 0;
			LOG_INFO("[%p]: START generation=%u", Device, Device->Generation);
			Device->RaopState = event;
			break;
		case RAOP_STOP:
			LOG_INFO("[%p]: Stop", Device);
			if (Device->CastSession != CAST_SESSION_IDLE && Device->CastSession != CAST_SESSION_STOPPING) {
				CastStop(Device->CastCtx);
				Device->ExpectStop = true;
			}
			Device->CastSession = CAST_SESSION_STOPPING;
			Device->RaopState = event;
			break;
		case RAOP_FLUSH:
			if (Device->Config.SoftFlush) {
				Device->SoftPausedAt = gettime_ms();
				Device->CastSession = CAST_SESSION_SOFT_PAUSED;
				Device->RaopState = event;
				LOG_INFO("[%p]: FLUSH -> SOFT_PAUSED generation=%u", Device, Device->Generation);
			} else if (Device->Config.Flush) {
				LOG_INFO("[%p]: Flush", Device);
				CastStop(Device->CastCtx);
				Device->ExpectStop = true;
				Device->CastSession = CAST_SESSION_STOPPING;
				Device->RaopState = event;
			}
			break;
		case RAOP_PLAY: {
			LOG_INFO("[%p]: RAOP first_audio +%ums", Device, gettime_ms() - Device->SessionStarted);
			Device->FirstAudioAt = gettime_ms();
			if (Device->CastSession == CAST_SESSION_SOFT_PAUSED && Device->Config.PersistentStream && *Device->StreamUri) {
				Device->CastSession = CAST_SESSION_RESUMING;
				Device->ResumeStartedAt = Device->FirstAudioAt;
				Device->FirstReadAt = 0;
				LOG_INFO("[%p]: RESUME after %ums generation=%u", Device, Device->FirstAudioAt - Device->SoftPausedAt, Device->Generation);
				CastPlayRetry(Device->CastCtx);
			} else if (Device->RaopState != RAOP_PLAY) {
				uint16_t port = va_arg(args, uint32_t);
				static int count;
				char codec[32] = "flac";
				(void) !sscanf(Device->Config.Codec, "%31[^:]", codec);
				snprintf(Device->StreamUri, sizeof(Device->StreamUri), "http://%s:%u/stream-%u.%s", inet_ntoa(glHost), port, count++, codec);
				Device->CastSession = CAST_SESSION_STARTING;
				Device->ExpectStop = false;
				LoadCurrentStream(Device);
				LOG_INFO("[%p]: CAST LOAD +%ums %s", Device, gettime_ms() - Device->SessionStarted, Device->StreamUri);
			}

			CastSetDeviceVolume(Device->CastCtx, Device->Volume, true);
			Device->RaopState = event;
			break;
		}
		case RAOP_VOLUME: {
			uint32_t now = gettime_ms();

			if (now > Device->VolumeStampRx + 1000) {
				Device->Volume = va_arg(args, double);
				Device->VolumeStampTx = now;
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);
				LOG_INFO("[%p]: Volume[0..1] %0.4lf", Device, Device->Volume);
			}
			break;
		}
		case RAOP_ARTWORK:
			// the body and len are sent as well but we don't use them
		case RAOP_METADATA: {
			if (Device->RaopState == RAOP_PLAY) {
				raopsr_metadata_t* raopMetaData = va_arg(args, raopsr_metadata_t*);
				struct metadata_s MetaData = { .title = raopMetaData->title,
											   .album = raopMetaData->album,
											   .artist = raopMetaData->artist,
											   .artwork = raopMetaData->artwork };
				CastPlay(Device->CastCtx, &MetaData);
			}
			break;
		}
		default:
			break;
	}

	va_end(args);
	pthread_mutex_unlock(&Device->Mutex);
}

/*----------------------------------------------------------------------------*/
#define TRACK_POLL  (1000)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args) {
	int elapsed, wakeTimer = TRACK_POLL;
	unsigned last = gettime_ms();
	struct sMR *p = (struct sMR*) args;
	json_t *data;

	while (p->Running) {
		double Volume = -1;

		// context is valid until this thread ends, no deletion issue
		data = GetTimedEvent(p->CastCtx, wakeTimer);
		if (!p->Running) break;

		elapsed = gettime_ms() - last;

		// need to protect against events from CC threads, not from deletion
		pthread_mutex_lock(&p->Mutex);

		wakeTimer = (p->State != STOPPED) ? TRACK_POLL : TRACK_POLL * 10;
		if (p->Config.RecoveryEnabled || p->CastSession == CAST_SESSION_SOFT_PAUSED) wakeTimer = 100;
		LOG_SDEBUG("[%p]: Cast thread timer %d %d", p, elapsed, wakeTimer);

		// a message has been received
		if (data) {
			json_t *generation = json_object_get(data, "_airconnect_generation");
			if (p->Config.RecoveryEnabled && json_is_integer(generation) &&
				(uint32_t) json_integer_value(generation) != p->Generation) {
				LOG_INFO("[%p]: ignoring stale Cast event generation=%u current=%u", p,
					(unsigned) json_integer_value(generation), p->Generation);
				json_decref(data);
				data = NULL;
			}
			if (data) {
			json_t *val = json_object_get(data, "type");
			const char *type = json_string_value(val);
			uint32_t now = gettime_ms();

			// a mediaSessionId has been acquired
			if (type && !strcasecmp(type, "MEDIA_STATUS")) {
				const char *state = GetMediaItem_S(data, 0, "playerState");

				if (state && !strcasecmp(state, "PLAYING") && p->State != PLAYING) {
					bool resumed = p->CastSession == CAST_SESSION_RESUMING;
					LOG_INFO("[%p]: Cast playing", p);
					p->State = PLAYING;
					p->CastSession = CAST_SESSION_PLAYING;
					p->ExpectStop = false;
					p->RetryCount = 0;
					p->CastPlayingAt = now;
					LOG_INFO("[%p]: CAST PLAYING +%ums startup=%ums generation=%u", p,
						now - p->FirstAudioAt, now - p->SessionStarted, p->Generation);
					if (resumed) {
						p->ResumeLatencyMs = now - p->ResumeStartedAt;
						LOG_INFO("[%p]: RESUME_LATENCY=%ums generation=%u", p, p->ResumeLatencyMs, p->Generation);
					}
					if (p->RaopState != RAOP_PLAY) raopsr_notify(p->Raop, RAOP_PLAY, NULL);
				}

				if (state && !strcasecmp(state, "PAUSED") && p->State == PLAYING) {
					LOG_INFO("[%p]: Cast pause", p);
					p->State = PAUSED;
					if (p->RaopState == RAOP_PLAY) raopsr_notify(p->Raop, RAOP_PAUSE, NULL);
				}

				if (state && !strcasecmp(state, "IDLE") && p->State != STOPPED) {
					const char *cause = GetMediaItem_S(data, 0, "idleReason");
					if (cause && !p->ExpectStop) {
						LOG_INFO("[%p]: Cast stopped by other remote", p);
						if (p->RaopState == RAOP_PLAY) raopsr_notify(p->Raop, RAOP_STOP, NULL);
						p->ExpectStop = false;
					}
					p->State = STOPPED;
					if (!p->ExpectStop) p->CastSession = CAST_SESSION_IDLE;
				}
			}

			// check for volume at the receiver level, but only record the change
			if (type && !strcasecmp(type, "RECEIVER_STATUS")) {
				double volume;
				bool muted;

				if (!p->Group && GetMediaVolume(data, 0, &volume, &muted)) {
					if (volume != -1 && !muted && volume != p->Volume) Volume = volume;
				}
			}

			// now apply the volume change if any
			if (Volume != -1 && fabs(Volume - p->Volume) >= 0.01 && now > p->VolumeStampTx + 1000) {
				p->VolumeStampRx = now;
				p->VolumeStampRx = now;
				LOG_INFO("[%p]: Volume local change %0.4lf", p, Volume);
				raopsr_notify(p->Raop, RAOP_VOLUME, &Volume);
				Volume = -1;
			}

			// always set volume done
			Volume = -1;

			// Cast devices has closed the connection
			if (type && !strcasecmp(type, "CLOSE")) {
				LOG_INFO("[%p]: Cast peer closed connection", p);
				p->State = STOPPED;
				if (p->Config.RecoveryEnabled && (p->RaopState == RAOP_PLAY || p->CastSession == CAST_SESSION_STARTING ||
					p->CastSession == CAST_SESSION_RESUMING || p->CastSession == CAST_SESSION_RECOVERING)) {
					StartRecovery(p, now, "Cast peer closed");
				} else {
					if (p->RaopState == RAOP_PLAY) raopsr_notify(p->Raop, RAOP_STOP, NULL);
					p->CastSession = CAST_SESSION_IDLE;
				}
			}

			json_decref(data);
			}
		}

		if (p->Config.RecoveryEnabled && p->CastSession != CAST_SESSION_FAILED &&
			p->CastSession != CAST_SESSION_STOPPING && p->CastSession != CAST_SESSION_IDLE) {
			raopsr_stats_t stats;
			uint32_t now = gettime_ms();
			if (raopsr_get_stats(p->Raop, &stats)) {
				if (stats.first_rtp_ms && !p->FirstRtpAt) {
					p->FirstRtpAt = stats.first_rtp_ms;
					LOG_INFO("[%p]: RAOP first_RTP +%ums", p, p->FirstRtpAt - p->SessionStarted);
				}
				if (stats.first_sync_ms && !p->FirstSyncAt) {
					p->FirstSyncAt = stats.first_sync_ms;
					LOG_INFO("[%p]: RAOP first_sync +%ums", p, p->FirstSyncAt - p->SessionStarted);
				}
				if (stats.last_read_ms && !p->FirstReadAt) {
					p->FirstReadAt = stats.last_read_ms;
					p->RetryCount = 0;
					LOG_INFO("[%p]: FIRST_READ +%ums%s fill=%u", p, p->FirstReadAt - p->SessionStarted,
						p->CastSession == CAST_SESSION_RESUMING ? " (resume)" : "", stats.fill);
				}
				uint32_t grace = p->Group && p->Config.GroupOptimized ? p->Config.GroupStartupGraceMs : 0;
				bool waiting = now - p->FirstAudioAt < grace;
				bool noGet = !stats.http_connected && now - p->FirstAudioAt >= p->Config.ReaderStallMs;
				bool noRead = stats.last_write_ms && now - stats.last_write_ms < p->Config.ReaderStallMs &&
					(!stats.last_read_ms || now - stats.last_read_ms >= p->Config.ReaderStallMs);
				bool resumeTimeout = p->CastSession == CAST_SESSION_RESUMING && p->Config.SoftFlushTimeoutMs &&
					now - p->ResumeStartedAt >= p->Config.SoftFlushTimeoutMs;
				if (!waiting && (noGet || noRead) && (p->CastSession != CAST_SESSION_RESUMING || resumeTimeout)) {
					StartRecovery(p, now, resumeTimeout ? "soft resume timed out" : (noGet ? "HTTP GET missing" : "HTTP reader stalled"));
				}
			}
		}

		// get track position & CurrentURI
		p->TrackPoll += elapsed;
		if (p->TrackPoll >= TRACK_POLL) {
			p->TrackPoll = 0;
			if (p->State != STOPPED) CastGetMediaStatus(p->CastCtx);
		}

		pthread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	list_clear((cross_list_t**)&p->GroupMaster, free);

	return NULL;
}

/*----------------------------------------------------------------------------*/
static char *GetmDNSAttribute(mdnssd_txt_attr_t *p, int count, char *name) {
	for (int j = 0; j < count; j++)
		if (!strcasecmp(p[j].name, name))
			return strdup(p[j].value);

	return NULL;
}

/*----------------------------------------------------------------------------*/
static struct sMR *SearchUDN(char *UDN) {
	for (int i = 0; i < glMaxDevices; i++) {
		if (glMRDevices[i].Running && !strcmp(glMRDevices[i].UDN, UDN))
			return glMRDevices + i;
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
static void UpdateDevices() {
	pthread_mutex_lock(&glMainMutex);

	for (int i = 0; i < glMaxDevices; i++) {
		struct sMR *Device = glMRDevices + i;
		if (Device->Running && Device->Remove && !CastIsConnected(Device->CastCtx)) {
			struct in_addr addr = CastGetAddr(glMRDevices[i].CastCtx);
			if (!ping_host(addr, 100)) {
				LOG_INFO("[%p]: removing renderer (%s)", Device, Device->Config.Name);
				raopsr_delete(Device->Raop);
				RemoveCastDevice(Device);
			} else {
				LOG_DEBUG("[%p]: (%s) mute to mDNS search, but answers ping, so keep it", Device, Device->Config.Name);
			}
		}
	}

	pthread_mutex_unlock(&glMainMutex);
}

/*----------------------------------------------------------------------------*/
static bool isMember(struct in_addr host) {
	for (int i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].Running && CastGetAddr(glMRDevices[i].CastCtx).s_addr == host.s_addr) return true;
	}
	return false;
}

/*----------------------------------------------------------------------------*/
static bool mDNSsearchCallback(mdnssd_service_t *slist, void *cookie, bool *stop) {
	struct sMR *Device;
	mdnssd_service_t *s;
	bool Updated = false;

	if (*loglevel == lDEBUG) {
		LOG_DEBUG("----------------- round ------------------", NULL);
		for (s = slist; s && glMainRunning; s = s->next) {
			char *host = strdup(inet_ntoa(s->host));
			LOG_DEBUG("[%s] host %s, srv %s, name %s ", s->expired  ? "EXPIRED" : "ACTIVE",
						host, inet_ntoa(s->addr), s->name);
			free(host);
		}
	}
	
	/*
	cast groups creation is difficult - as storm of mDNS message is sent during
	master's election and many masters will claim the group then will "retract"
	one by one. The logic below works well if no announce is missed, which is
	not the case under high traffic, so in that case, either the actual master
	is missed and it will be discovered at the next 20s search or some retractions
	are missed and if the group is destroyed right after creation, then it will
	hang around	until the retractations timeout (2mins) - still correct as the
	end result is with the right master and group is ultimately removed, but not
	very user-friendy
	*/

	for (s = slist; s && glMainRunning; s = s->next) {
		char *UDN = NULL, *Name = NULL;
		char *Model;
		bool Group;

		// is the mDNS record usable or announce made on behalf
		if ((UDN = GetmDNSAttribute(s->attr, s->attr_count, "id")) == NULL || (s->host.s_addr != s->addr.s_addr && isMember(s->host))) continue;

		// is that device already here
		if ((Device = SearchUDN(UDN)) != NULL) {
			// a service is being removed
			Device->Remove = s->expired;
			if (s->expired) {
				// groups need to find if the removed service is the master
				if (Device->Group) {
					// there are some other master candidates
					if (Device->GroupMaster->Next) {
						Device->Remove = false;
						// changing the master, so need to update cast params
						if (Device->GroupMaster->Host.s_addr == s->host.s_addr) {
							free(list_pop((cross_list_t**) &Device->GroupMaster));
							UpdateCastDevice(Device->CastCtx, Device->GroupMaster->Host, Device->GroupMaster->Port);
						} else {
							struct sGroupMember *Member = Device->GroupMaster;
							while (Member && (Member->Host.s_addr != s->host.s_addr)) Member = Member->Next;
							if (Member) free(list_remove((cross_list_t*) Member, (cross_list_t**) &Device->GroupMaster));
						}
					}
				}
				if (Device->Remove && ping_host(s->addr, 100)) {
					LOG_INFO("[%p]: %s mute to mDNS search, but answers ping, so keep it", Device, Device->Config.Name);
				}
			// device update - when playing ChromeCast update their TXT records
			} else {
				char *Name = GetmDNSAttribute(s->attr, s->attr_count, "fn");

				// new master in election, update and put it in the queue
				if (Device->Group && Device->GroupMaster->Host.s_addr != s->addr.s_addr) {
					struct sGroupMember *Member = calloc(1, sizeof(struct sGroupMember));
					Member->Host = s->host;
					Member->Port = s->port;
					list_push((cross_list_t*) Member, (cross_list_t**) &Device->GroupMaster);
				}
				
				UpdateCastDevice(Device->CastCtx, s->addr, s->port);
				
				// update Device name if needed
				if (Name && strcmp(Name, Device->Name)) {
					char* autoName = NULL;
					(void)!asprintf(&autoName, glNameFormat, Device->Name);
					if (!strcmp(autoName, Device->Config.Name)) {
						LOG_INFO("[%p]: Device name change %s %s", Device, Name, Device->Name);
						raopsr_update(Device->Raop, Name, "aircast");
						strcpy(Device->Name, Name);
						sprintf(Device->Config.Name, glNameFormat, Name);
						Updated = true;
					}
					NFREE(autoName);
				}
				NFREE(Name);
			}
			NFREE(UDN);
			continue;
		}

		// disconnect of an unknown device
		if (!s->port && !s->addr.s_addr) {
			LOG_ERROR("Unknown device disconnected %s", s->name);
			NFREE(UDN);
			continue;
		}

		// new device so search a free spot - as this function is not called
		// recursively, no need to lock the device's mutex
		for (Device = glMRDevices; Device < glMRDevices + glMaxDevices && Device->Running; Device++);

		// no more room !
		if (Device == glMRDevices + glMaxDevices) {
			LOG_ERROR("Too many devices (max:%u)", glMaxDevices);
			NFREE(UDN);
			break;
		}
		
		// if model is a group
		Model = GetmDNSAttribute(s->attr, s->attr_count, "md");
		if (Model && !strcasestr(Model, "Group")) Group = false;
		else Group = true;
		NFREE(Model);

		Name = GetmDNSAttribute(s->attr, s->attr_count, "fn");
		if (!Name) Name = strdup(s->hostname);
		
		if (AddCastDevice(Device, Name, UDN, Group, s->addr, s->port) && !glDiscovery) {
			Device->Raop = raopsr_create(glHost, glmDNSServer, Device->Config.Name,
										"aircast", Device->Config.mac, Device->Config.Codec,
										Device->Config.Metadata, Device->Config.Drift,
										Device->Config.Flush, Device->Config.Latency,
										Device, raop_cb, raop_http_cb, glPortBase, glPortRange, -1);
			if (Device->Raop) raopsr_set_stream_options(Device->Raop, Device->Config.SoftFlush, Device->Config.PrebufferMs);
			if (!Device->Raop) {
				LOG_ERROR("[%p]: cannot create RAOP instance (%s)", Device, Device->Config.Name);
				RemoveCastDevice(Device);
			} else {
				Updated = true;
			}
		}

		NFREE(UDN);
		NFREE(Name);
	}

	UpdateDevices();

	if ((Updated && glAutoSaveConfigFile) || glDiscovery) {
		LOG_INFO("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, false);
	}

	// we have not released the slist
	return false;
}

/*----------------------------------------------------------------------------*/
static void *mDNSsearchThread(void *args) {
	// launch the query,
	mdnssd_query(glmDNSsearchHandle, "_googlecast._tcp.local", false,
			   glDiscovery ? DISCOVERY_TIME : 0, &mDNSsearchCallback, NULL);
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void *MainThread(void *args) {
	while (glMainRunning) {
		crossthreads_sleep(30*1000);
		if (!glMainRunning) break;

		if (glLogFile && glLogLimit != - 1) {
			uint32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				uint32_t Sum, BufSize = 16384;
				uint8_t *buf = malloc(BufSize);

				FILE *rlog = fopen(glLogFile, "rb");
				FILE *wlog = fopen(glLogFile, "r+b");
				LOG_DEBUG("Resizing log", NULL);
				for (Sum = 0, fseek(rlog, size - (glLogLimit*1024*1024) / 2, SEEK_SET);
					 (BufSize = fread(buf, 1, BufSize, rlog)) != 0;
					 Sum += BufSize, fwrite(buf, 1, BufSize, wlog));

				Sum = fresize(wlog, Sum);
				fclose(wlog);
				fclose(rlog);
				NFREE(buf);
				if (!freopen(glLogFile, "a", stderr)) {
					LOG_ERROR("re-open error while truncating log", NULL);
				}
			}
		}

		// try to detect IP change when not forced
		if (inet_addr(glBinding) == INADDR_NONE) {
			struct in_addr host;
			host = get_interface(!strchr(glBinding, '?') ? glBinding : NULL, NULL, &glNetmask);
			if (host.s_addr != INADDR_NONE && host.s_addr != glHost.s_addr) {
				LOG_INFO("IP change detected %s", inet_ntoa(glHost));
				Stop(false);
				glMainRunning = true;
				Start(false);
			}
		}

		UpdateDevices();
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
static bool AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool group, struct in_addr ip, uint16_t port) {
	// read parameters from default then config file
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	LoadMRConfig(glConfigID, UDN, &Device->Config);
	if (!Device->Config.Enabled) return false;
	if (Device->Config.GroupOptimized && !group) Device->Config.ReaderStallMs *= 2;

	// do not zero-out the structure as the mutex must be preserved
	strcpy(Device->UDN, UDN);
	Device->Magic		= MAGIC;
	Device->Running		= true;
	Device->State 		= STOPPED;
	Device->ExpectStop 	= false;
	Device->Volume 		= Device->Elapsed = Device->TrackPoll = 0;
	Device->CastCtx 	= NULL;
	Device->Raop 		= NULL;
	Device->RaopState	= RAOP_STOP;
	Device->Group 		= group;
	Device->Remove		= false;
	Device->CastSession = CAST_SESSION_IDLE;
	Device->Generation = Device->SessionStarted = Device->SoftPausedAt = Device->ResumeStartedAt = Device->LastRecoveryAt = 0;
	Device->RetryCount = Device->ReaderStalls = Device->Reloads = 0;
	Device->TotalRetries = Device->TotalReaderStalls = Device->TotalReloads = 0;
	Device->FirstRtpAt = Device->FirstSyncAt = Device->FirstAudioAt = Device->HttpGetAt = Device->FirstReadAt = Device->CastPlayingAt = 0;
	Device->ResumeLatencyMs = 0;
	Device->StreamUri[0] = '\0';
	Device->VolumeStampRx = Device->VolumeStampTx = gettime_ms() - 2000;

	if (group) {
		Device->GroupMaster	= calloc(1, sizeof(struct sGroupMember));
		Device->GroupMaster->Host = ip;
		Device->GroupMaster->Port = port;
	} else Device->GroupMaster = NULL;

	if (!*Device->Config.Name) sprintf(Device->Config.Name, glNameFormat, Name);
	strcpy(Device->Name, Name);

	if (!memcmp(Device->Config.mac, "\0\0\0\0\0\0", 6)) {
		uint32_t mac_size = 6;
		if (group || SendARP(ip.s_addr, INADDR_ANY, Device->Config.mac, &mac_size)) {
			*(uint32_t*) (Device->Config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: creating MAC", Device);
		}
		memset(Device->Config.mac, 0xcc, 2);
	}

	// virtual players duplicate mac address
	for (int i = 0; i < glMaxDevices; i++) {
		if (glMRDevices[i].Running && Device != glMRDevices + i && !memcmp(&glMRDevices[i].Config.mac, Device->Config.mac, 6)) {
			memset(Device->Config.mac, 0xcc, 2);
			*(uint32_t*) (Device->Config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: duplicated mac ... updating", Device);
		}
	}

	LOG_INFO("[%p]: adding renderer (%s - %s:%hu) with mac %hX%X", Device, Name, inet_ntoa(ip), port, *(uint16_t*) Device->Config.mac, *(uint32_t*) (Device->Config.mac + 2));

	Device->CastCtx = CreateCastDevice(Device, Device->Group, Device->Config.StopReceiver, ip, port, Device->Config.MediaVolume);
	CastSetPlayDedupe(Device->CastCtx, Device->Config.PlayDedupeMs);
	pthread_create(&Device->Thread, NULL, &MRThread, Device);

	return true;
}

/*----------------------------------------------------------------------------*/
static void FlushCastDevices(void) {
	for (int i = 0; i < glMaxDevices; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->Running) {
			raopsr_delete(p->Raop);
			RemoveCastDevice(p);
		 }
	}
}

/*----------------------------------------------------------------------------*/
static void RemoveCastDevice(struct sMR *Device) {
	pthread_mutex_lock(&Device->Mutex);
	Device->Running = false;
	pthread_mutex_unlock(&Device->Mutex);
	
	// device's thread can still be running but this will wake it up and end it
	DeleteCastDevice(Device->CastCtx);

	pthread_join(Device->Thread, NULL);
}

/*----------------------------------------------------------------------------*/
static bool Start(bool cold) {
	// must bind to an address
	char* iface = NULL;
	glHost = get_interface(!strchr(glBinding, '?') ? glBinding : NULL, &iface, &glNetmask);
	LOG_INFO("Binding to %s [%s] with mask 0x%08x", inet_ntoa(glHost), iface, ntohl(glNetmask));
	NFREE(iface);

	// can't find a suitable interface
	if (glHost.s_addr == INADDR_NONE) return false;

	if (cold) {
		// manually load openSSL symbols to accept multiple versions
		if (!cross_ssl_load()) {
			LOG_ERROR("Cannot load SSL libraries", NULL);
			return false;
		}

		// mutexes must always be valid
		glMRDevices = calloc(glMaxDevices, sizeof(struct sMR));
		for (int i = 0; i < glMaxDevices; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);

		pthread_mutex_init(&glMainMutex, 0);

		// start the main thread
		pthread_create(&glMainThread, NULL, &MainThread, NULL);
	}

	// init pico httpserver
	glPicoPort = glPortBase;
	http_pico_init(glHost, &glPicoPort, glPicoPort ? glPortRange : 1);
	LOG_INFO("Starting pico HTTP server on port %hu", glPicoPort);

	char hostname[STR_LEN];
	gethostname(hostname, sizeof(hostname));
	strcat(hostname, ".local");

	if ((glmDNSServer = mdnsd_start(glHost, false)) == NULL) return false;
	mdnsd_set_hostname(glmDNSServer, hostname, glHost);

	// start the mDNS devices discovery thread
	glmDNSsearchHandle = mdnssd_init(false, glHost, true);
	pthread_create(&glmDNSsearchThread, NULL, &mDNSsearchThread, NULL);
	if (!StartMetrics()) LOG_ERROR("Cannot start metrics server", NULL);

	return true;
}

/*---------------------------------------------------------------------------*/
static bool Stop(bool exit) {
	glMainRunning = false;
	StopMetrics();

	if (glHost.s_addr != INADDR_ANY) {
		LOG_DEBUG("terminate search thread ...", NULL);
		// this forces an ongoing search to end
		mdnssd_close(glmDNSsearchHandle);
		pthread_join(glmDNSsearchThread, NULL);

		LOG_DEBUG("flush renderers ...", NULL);
		FlushCastDevices();

		// stop broadcasting devices
		mdnsd_stop(glmDNSServer);
	}

	if (exit) {
		LOG_DEBUG("terminate main thread ...", NULL);
		crossthreads_wake();
		pthread_join(glMainThread, NULL);
		for (int i = 0; i < glMaxDevices; i++) pthread_mutex_destroy(&glMRDevices[i].Mutex);
		pthread_mutex_destroy(&glMainMutex);

		// terminate pico http server
		http_pico_close();

		if (glConfigID) ixmlDocument_free(glConfigID);
		netsock_close();
		cross_ssl_free();
	}

	free(glMRDevices);
	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	if (!glGracefullShutdown) {
		for (int i = 0; i < glMaxDevices; i++) {
			struct sMR *p = &glMRDevices[i];
			if (p->Running && p->State == PLAYING) CastStop(p->CastCtx);
		}
		LOG_INFO("forced exit", NULL);
		exit(0);
	}

	Stop(true);
	exit(0);
}

/*---------------------------------------------------------------------------*/
static bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1;
	char cmdline[256] = "";

	for (int i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < sizeof(cmdline)); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if ((!strcmp(opt, "-profile") || !strcmp(opt, "-metrics-port")) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("abxdpiflcvN", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIkr", opt) || opt[0] == '-') {
			optarg = NULL;
			optind += 1;
		}
		else {
			printf("%s", usage);
			return false;
		}

		switch (opt[0]) {
		case 'f':
			glLogFile = optarg;
			break;
		case 'v':
			glMRConfig.MediaVolume = atof(optarg);
			break;
		case 'c':
			strcpy(glMRConfig.Codec, optarg);
			break;
		case 'b':
			strcpy(glBinding, optarg);
			break;
		case 'a':
			sscanf(optarg, "%hu:%hu", &glPortBase, &glPortRange);
			break;
		case 'i':
			strcpy(glConfigName, optarg);
			glDiscovery = true;
			break;
		case 'I':
			glAutoSaveConfigFile = true;
			break;
		case 'p':
			glPidFile = optarg;
			break;
		case 'N':
			glNameFormat = optarg;
			break;
		case 'Z':
			glInteractive = false;
			break;
		case 'k':
			glGracefullShutdown = false;
			break;
		case 'r':
			glMRConfig.Drift = true;
			break;
		case 'l':
			strcpy(glMRConfig.Latency, optarg);
			break;
#if LINUX || FREEBSD
		case 'z':
			glDaemonize = true;
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "error"))  new = lERROR;
					if (!strcmp(v, "warn"))   new = lWARN;
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "main")) main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util")) util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "cast")) cast_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "raop")) raop_loglevel = new;
				}
				else {
					printf("%s", usage);
					return false;
				}
			}
			break;
		case 't':
			printf("%s", license);
			return false;
		case '-':
			if (!strcmp(opt + 1, "noflush")) glMRConfig.Flush = false;
			else if (!strcmp(opt + 1, "profile") && !strcmp(optarg, "music-assistant") && !glMusicAssistantProfile) {
				ApplyMusicAssistantProfile(&glMRConfig);
				glMusicAssistantProfile = true;
			}
			else if (!strcmp(opt + 1, "metrics-port")) glMetricsPort = strtoul(optarg, NULL, 10);
			else { printf("%s", usage); return false; }
			break;
		default:
			break;
		}
	}

	return true;
}

/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[]) {
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif
#if defined(SIGPIPE)
	signal(SIGPIPE, SIG_IGN);
#endif

	// otherwise some atof/strtod fail with '.'
	setlocale(LC_NUMERIC, "C");

	netsock_init();

	// Apply profile defaults before config.xml so global and per-device values can tune the preset.
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--profile") && i + 1 < argc && !strcmp(argv[i + 1], "music-assistant")) {
			ApplyMusicAssistantProfile(&glMRConfig);
			glMusicAssistantProfile = true;
		}
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	// make sure port range is correct
	if (glPortBase && !glPortRange) glPortRange = glMaxDevices*4;

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_WARN("Starting aircast version: %s", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_WARN("weird GLIBC, try -static build in case of failure");
	}

	if (!glConfigID) {
		LOG_WARN("no config file, using defaults");
	}

	// just do device discovery and exit
	if (glDiscovery) {
		Start(true);
		sleep(DISCOVERY_TIME + 1);
		Stop(true);
		return(0);
	}

#if LINUX || FREEBSD
	if (glDaemonize) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

	if (glPidFile) {
		FILE *pid_file;
		pid_file = fopen(glPidFile, "wb");
		if (pid_file) {
			fprintf(pid_file, "%d", (int) getpid());
			fclose(pid_file);
		}
		else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}

	if (!Start(true)) {

		LOG_ERROR("Cannot start", NULL);

		exit(1);

	}

	for (char resp[20] = ""; strcmp(resp, "exit");) {
#if LINUX || FREEBSD || SUNOS
		if (!glDaemonize && glInteractive)
			(void)! scanf("%s", resp);
		else
			pause();
#else
		if (glInteractive)
			(void)! scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif
		char level[20];

		if (!strcmp(resp, "maindbg"))	{
			(void)! scanf("%s", level);
			main_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "utildbg"))	{
			(void)! scanf("%s", level);
			util_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "castdbg"))	{
			(void)! scanf("%s", level);
			cast_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "raopdbg"))	{
			(void)! scanf("%s", level);
			raop_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "save"))	{
			char name[128];
			(void)! scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}

		if (!strcmp(resp, "dump") || !strcmp(resp, "dumpall"))	{
			bool all = !strcmp(resp, "dumpall");

			for (int i = 0; i < glMaxDevices; i++) {
				struct sMR *p = &glMRDevices[i];
				bool Locked = pthread_mutex_trylock(&p->Mutex);

				if (!Locked) pthread_mutex_unlock(&p->Mutex);
				if (!p->Running && !all) continue;
				printf("%20.20s [r:%u] [l:%u] [s:%u]", p->Config.Name, p->Running,
					   Locked, p->State);
				if (p->Group)
					printf(" [m:%p, n:%p]\n", p->GroupMaster,
						   p->GroupMaster ? p->GroupMaster->Next : NULL);
				printf("\n");
			}
		}

	};

	LOG_INFO("stopping Cast devices ...", NULL);
	Stop(true);
	LOG_INFO("all done", NULL);

	return true;
}




