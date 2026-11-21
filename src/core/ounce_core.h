// Hooks that tools/patch_chiaki.py adds to chiaki-ng's streaming code.
#ifndef OUNCE_CORE_H
#define OUNCE_CORE_H

#include <chiaki/bitstream.h>
#include <chiaki/common.h>
#include <chiaki/log.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct chiaki_frame_processor_t;

// The socket receive buffer Takion asks for.
#define OUNCE_RECEIVE_BUFFER (1024 * 1024)
// Sets SO_RCVBUF to OUNCE_RECEIVE_BUFFER, halving on refusal down to minimum.
// Returns the size the socket reports afterwards, or -1 if nothing was accepted.
// log may be NULL.
int ounce_set_receive_buffer(int sock, int minimum, ChiakiLog *log);

// Takion's receive loop reports each wait for data (monotonic microseconds).
void ounce_note_receive_wait(uint64_t start_us, uint64_t end_us);
// How long the socket has had data waiting at every wait; 0 when it was empty.
int ounce_receive_backlog_ms(void);
void ounce_note_congestion_signal(void);

// Set by the video receiver around a flush while it waits for an IDR frame.
extern ChiakiBitstream *ounce_fec_skip_bitstream;
int ounce_frame_skip_fec(struct chiaki_frame_processor_t *frame_processor);

// Returns 0 when a corrupt-frame report is covered by one sent < 100 ms ago.
int ounce_corrupt_report_due(uint16_t start, uint64_t now_ms);

typedef struct OunceCoreStats
{
	uint64_t congestion_signals; // reports raised to packet_loss_max by the backlog signal
	uint64_t fec_skipped;        // P-frames not repaired while waiting for an IDR
	uint64_t reports_coalesced;  // corrupt-frame reports covered by an earlier one
	int backlog_ms;              // current receive backlog
} OunceCoreStats;

OunceCoreStats ounce_core_stats(void);
void ounce_core_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif
