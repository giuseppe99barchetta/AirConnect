# Music Assistant / Cast Group profile

## What upstream does

`aircast` receives RAOP callbacks under the device mutex.  On `RAOP_FLUSH` it
sends `STOP`; the next non-silent audio packet causes `LOAD` followed by
`PLAY`.  The RAOP streamer in `common/libraop` also clears its frame ring and
closes its per-session HTTP socket on that flush.  Consequently, merely
skipping `CastStop` cannot retain a usable Cast session.

The RAOP streamer owns the HTTP connection and the W/R frame ring.  Cast I/O
has its own socket and queue threads.  `MRThread` is the sole coordinator: it
observes Cast events and the read-only RAOP statistics, then requests recovery
while holding the existing device mutex.  It never blocks or sleeps.

## State machine

The optional profile adds a state independent of upstream RAOP and Cast
values:

`IDLE -> STARTING -> PLAYING -> SOFT_PAUSED -> RESUMING -> PLAYING`.

`STOPPING`, `RECOVERING`, and `FAILED` are terminal/intermediate recovery
states.  A real `TEARDOWN` always goes to `STOPPING`; a soft flush only enters
`SOFT_PAUSED`.  A generation is incremented for every real start and recovery
load, so timestamps and delayed status from an earlier generation are ignored.

## Recovery

When W advances while R has not advanced for `cast_reader_stall_ms`, and the
session is starting or playing, recovery is bounded: forced `PLAY`, then
`STOP/LOAD/PLAY`, then receiver reconnect.  Retry count is capped.  The group
profile only changes these timeouts and prefers the first two stages; it does
not change the default upstream path.

The soft-flush timeout starts only after a `RAOP_PLAY` resume request; a long
pause retains the session.  Cast events are tagged with the active generation
before they enter the device event queue, and older generations are discarded.
`--metrics-port` exports the per-device recovery and latency counters on
`/metrics`.

## Timeline discontinuities and live-edge recovery

The RAOP control connection, RTP producer, local PCM ring, Cast HTTP consumer
and Cast media session are separate lifetimes.  A closed RTSP TCP connection
is therefore evidence, not by itself a reason to stop Cast: RTP can continue
after it closes.  Conversely, a closed Cast HTTP connection while fresh RTP
is still arriving is a real Cast media discontinuity, not a normal `PLAY`
retry.

The Music Assistant profile records both sides of each session (control/RTP
timestamps, HTTP open/close, frames served, media status and generation).  A
source is considered live only while fresh decoded RTP audio is arriving.  If
the RTSP control socket closes and that audio later becomes stale, the source
timeline is marked discontinuous, Cast is stopped, and the process waits for
a new RAOP generation.  It never keeps feeding old buffered audio.

If Cast closes its HTTP media connection while the RAOP source remains live,
the profile performs a hard media resync: it invalidates the old output
generation, discards queued PCM and pending transport commands, assigns a new
stream URI/generation, and sends a fresh `LOAD`.  The live edge is the RAOP
ring write head at the instant of invalidation; all frames before it are
discarded, so the next Cast HTTP GET can receive only current/future source
audio.  This deliberately favours a short audible recovery over resuming an
old Cast buffer at a new permanent offset.

Metadata is not part of that transport state.  The Default Media Receiver has
no safe metadata-only update used by this fork, so the Music Assistant profile
does not send metadata as a Cast `PLAY`.  Upstream behaviour remains available
outside the profile.

## Risks and boundaries

The RAOP HTTP listener is created for every real RTSP session, so a URL cannot
be safely retained across `TEARDOWN` without a separate long-lived HTTP
server.  The profile therefore preserves the existing URL and socket only
across a soft flush; this avoids stale audio by resetting the ring generation.
No sample-accurate AirPlay/Cast synchronization is claimed.

Clock-drift measurement and PCM rate correction are intentionally deferred.
They are meaningful only after a continuous session can be demonstrated with
no RTSP-source loss, Cast HTTP reconnect, hard resync, or metadata-triggered
transport command.
