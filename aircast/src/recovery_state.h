#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool RecoveryCounterAdvanced(uint32_t *seen, uint32_t current) {
	bool advanced = current != *seen;
	*seen = current;
	return advanced;
}

/* A decoded ALAC frame is the first reliable proof that the live edge is playable. */
static inline bool RecoverySourcePlayable(uint32_t now, uint32_t last_decoded_ms, uint32_t stall_ms) {
	return last_decoded_ms && (!stall_ms || now - last_decoded_ms < stall_ms);
}

static inline bool RecoveryShouldHardResync(bool http_closed, bool resync_in_progress,
		bool source_playable, bool raop_playing) {
	return http_closed && !resync_in_progress && source_playable && raop_playing;
}
