/*
 *  AirCast: Chromecast to AirPlay
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#pragma once

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>

#include "platform.h"
#include "pthread.h"
#include "raop_server.h"
#include "cast_util.h"

#define VERSION "v1.11.2"" ("__DATE__" @ "__TIME__")"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define STR_LEN	256

#define MAX_PROTO		128
#define MAX_RENDERERS	32
#define	AV_TRANSPORT 	"urn:schemas-upnp-org:service:AVTransport:1"
#define	RENDERING_CTRL 	"urn:schemas-upnp-org:service:RenderingControl:1"
#define	CONNECTION_MGR 	"urn:schemas-upnp-org:service:ConnectionManager:1"
#define MAGIC			0xAABBCCDD
#define RESOURCE_LENGTH	250
#define	SCAN_TIMEOUT 	15
#define SCAN_INTERVAL	30

enum 	eMRstate { STOPPED, PLAYING, PAUSED };
typedef enum { CAST_SESSION_IDLE, CAST_SESSION_STARTING, CAST_SESSION_PLAYING,
	CAST_SESSION_SOFT_PAUSED, CAST_SESSION_RESUMING, CAST_SESSION_STOPPING,
	CAST_SESSION_RECOVERING, CAST_SESSION_FAILED } cast_session_state_t;

typedef struct sMRConfig
{
	bool		Enabled;
	bool		StopReceiver;
	char		Name[STR_LEN];
	char		Codec[STR_LEN];
	bool		Metadata;
	bool		Flush;
	double		MediaVolume;
	uint8_t		mac[6];
	char		Latency[STR_LEN];
	bool		Drift;
	char		ArtWork[4*STR_LEN];
	bool		SoftFlush;
	uint32_t	SoftFlushTimeoutMs, ReaderStallMs, PlayRetryMs, MaxRetries;
	bool		GroupOptimized;
	uint32_t	PrebufferMs;
	bool		PersistentStream;
	uint32_t	PlayDedupeMs;
	bool		RecoveryEnabled;
	uint32_t	GroupStartupGraceMs;
	bool		DiscontinuityRecovery;
	uint32_t	SourceStallMs;
	bool		MetadataTransportUpdates;
} tMRConfig;

struct sMR {
	uint32_t Magic;
	bool  Running;
	tMRConfig Config;
	struct raopsr_s *Raop;
	raopsr_event_t	RaopState;
	char UDN	   	[RESOURCE_LENGTH];
	char Name		[STR_LEN];
	enum eMRstate 	State;
	bool			ExpectStop;
	uint32_t			Elapsed;
	unsigned		TrackPoll;
	void			*CastCtx;
	pthread_mutex_t Mutex;
	pthread_t 		Thread;
	double			Volume;
	uint32_t			VolumeStampRx, VolumeStampTx;
	bool			Group;
	struct sGroupMember {
		struct sGroupMember	*Next;
		struct in_addr		Host;
		uint16_t				Port;
   } *GroupMaster;
   bool Remove;
	cast_session_state_t CastSession;
	uint32_t Generation, SessionStarted, SoftPausedAt, ResumeStartedAt, LastRecoveryAt;
	uint32_t RetryCount, ReaderStalls, Reloads;
	uint32_t TotalRetries, TotalReaderStalls, TotalReloads, TotalDiscontinuities;
	uint32_t FirstRtpAt, FirstSyncAt, FirstAudioAt, HttpGetAt, FirstReadAt, CastPlayingAt;
	uint32_t ResumeLatencyMs;
	uint32_t CastLoadAt, CastCurrentTimeAt;
	double CastCurrentTimeMs;
	uint32_t RaopHttpPort;
	uint32_t SeenControlDisconnects, SeenHttpDisconnects, SeenHttpReconnects, SeenDiscontinuities;
	bool SourceControlClosed, SourceDiscontinuous;
	char StreamUri[STR_LEN];
};

extern int32_t				glLogLimit;
extern tMRConfig			glMRConfig;
extern struct sMR			*glMRDevices;
extern int					glMaxDevices;
extern unsigned short		glPortBase, glPortRange;
extern char					glBinding[16];
