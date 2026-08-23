#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { KEEP_PLAYING, HARD_RESYNC, STOP_FOR_SOURCE_LOSS } action_t;

typedef struct {
	uint16_t write, read;
	uint32_t cached_bytes, generation;
	bool metadata_transport_updates;
} stream_t;

static action_t decide(bool control_closed, bool source_live, bool http_closed, bool playing) {
	if (control_closed && !source_live) return STOP_FOR_SOURCE_LOSS;
	if (http_closed && source_live && playing) return HARD_RESYNC;
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
	return 0;
}
