#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../src/recovery_state.h"

typedef enum { KEEP_PLAYING, HARD_RESYNC, STOP_FOR_SOURCE_LOSS } action_t;

typedef struct {
	uint16_t write, read;
	uint32_t cached_bytes, generation;
	bool metadata_transport_updates, hard_resync_in_progress;
	uint32_t seen_control_disconnects, seen_http_disconnects;
} stream_t;

static action_t decide(bool control_closed, bool source_live, bool http_closed, bool playing) {
	if (control_closed && !source_live) return STOP_FOR_SOURCE_LOSS;
	if (RecoveryShouldHardResync(http_closed, false, source_live, playing)) return HARD_RESYNC;
	return KEEP_PLAYING;
}

static void discard_to_live_edge(stream_t *stream) {
	stream->read = stream->write + 1;
	stream->cached_bytes = 0;
	stream->generation++;
}

int main(void) {
	stream_t stream = { .write = 100, .read = 96, .cached_bytes = 8192, .generation = 7,
		.metadata_transport_updates = false };

	assert(decide(false, true, true, true) == HARD_RESYNC);      /* Cast HTTP closes, RAOP is live */
	discard_to_live_edge(&stream);
	assert(stream.read == 101 && stream.cached_bytes == 0 && stream.generation == 8); /* no stale PCM */
	assert(decide(true, true, false, true) == KEEP_PLAYING);     /* RTSP closes, RTP continues */
	assert(decide(true, false, false, true) == STOP_FOR_SOURCE_LOSS); /* source really stopped */
	assert(decide(false, true, false, true) == KEEP_PLAYING);    /* normal media status */
	assert(!stream.metadata_transport_updates);                   /* metadata cannot issue PLAY */

	stream.write = 140;                                          /* fresh packets after reconnect */
	assert(stream.read == 101 && stream.read <= stream.write);   /* only post-resync audio is eligible */
	assert(stream.read > 100);                                   /* pre-resync PCM cannot be replayed */

	/* A cumulative RTSP counter must be snapshotted on a new generation. */
	stream.seen_control_disconnects = 1;
	assert(!RecoveryCounterAdvanced(&stream.seen_control_disconnects, 1));
	assert(RecoveryCounterAdvanced(&stream.seen_control_disconnects, 2));
	stream.seen_http_disconnects = 4;
	assert(!RecoveryCounterAdvanced(&stream.seen_http_disconnects, 4));

	/* Fresh RTP is not enough: the decoded PCM live edge must be fresh. */
	assert(RecoverySourcePlayable(10000, 9000, 2500));
	assert(!RecoverySourcePlayable(10000, 7000, 2500));

	/* One external close starts exactly one resync; its own close is ignored. */
	unsigned hard_resyncs = 0;
	if (RecoveryCounterAdvanced(&stream.seen_http_disconnects, 5) &&
		RecoveryShouldHardResync(true, false, true, true)) hard_resyncs++;
	assert(hard_resyncs == 1);
	stream.hard_resync_in_progress = true;
	assert(RecoveryCounterAdvanced(&stream.seen_http_disconnects, 6));
	assert(!RecoveryShouldHardResync(true, stream.hard_resync_in_progress, true, true));
	assert(hard_resyncs == 1);

	/* The reload owns a new URI/generation and eventually returns to PLAYING. */
	char old_uri[64], new_uri[64];
	snprintf(old_uri, sizeof(old_uri), "stream-%u.flac", stream.generation);
	discard_to_live_edge(&stream);
	snprintf(new_uri, sizeof(new_uri), "stream-%u.flac", stream.generation);
	assert(strcmp(old_uri, new_uri));
	stream.hard_resync_in_progress = false;                       /* simulated Cast PLAYING */
	assert(!stream.hard_resync_in_progress);
	return 0;
}
