#include <assert.h>

typedef enum { IDLE, STARTING, PLAYING, SOFT_PAUSED, RESUMING, STOPPING, RECOVERING, FAILED } state_t;
typedef enum { START, FLUSH, PLAY, STOP, STALL, CLOSE, TEARDOWN } event_t;

static state_t next(state_t state, event_t event, int retries, int max_retries) {
	if (event == TEARDOWN || event == STOP) return STOPPING;
	if (event == CLOSE) return IDLE;
	if (event == START) return STARTING;
	if (event == FLUSH && state == PLAYING) return SOFT_PAUSED;
	if (event == PLAY && state == SOFT_PAUSED) return RESUMING;
	if (event == PLAY && (state == STARTING || state == RESUMING)) return PLAYING;
	if (event == STALL) return retries >= max_retries ? FAILED : RECOVERING;
	return state;
}

int main(void) {
	assert(next(IDLE, START, 0, 3) == STARTING);                 /* normal start */
	assert(next(STARTING, PLAY, 0, 3) == PLAYING);
	assert(next(PLAYING, FLUSH, 0, 3) == SOFT_PAUSED);           /* pause/resume */
	assert(next(SOFT_PAUSED, PLAY, 0, 3) == RESUMING);
	assert(next(RESUMING, PLAY, 0, 3) == PLAYING);
	assert(next(PLAYING, FLUSH, 0, 3) == SOFT_PAUSED);           /* repeated flush */
	assert(next(PLAYING, STALL, 0, 3) == RECOVERING);            /* HTTP stall */
	assert(next(RECOVERING, STALL, 3, 3) == FAILED);             /* retry bound */
	assert(next(STARTING, CLOSE, 0, 3) == IDLE);                 /* close startup */
	assert(next(PLAYING, CLOSE, 0, 3) == IDLE);                  /* close playback */
	assert(next(RECOVERING, TEARDOWN, 0, 3) == STOPPING);        /* teardown recovery */
	return 0;
}
