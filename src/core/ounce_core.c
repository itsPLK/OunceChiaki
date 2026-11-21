// Streaming behavior carried over from CivChiaki; called from the patched
// chiaki-ng sources (tools/patch_chiaki.py). See docs/STREAMING.md.
#include "ounce_core.h"

#include <chiaki/frameprocessor.h>
#include <chiaki/time.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/socket.h>

// Private to frameprocessor.c; patch_chiaki.py fails if its layout changes.
struct chiaki_frame_unit_t
{
	size_t data_size;
};

int ounce_set_receive_buffer(int sock, int minimum, ChiakiLog *log)
{
	int requested = OUNCE_RECEIVE_BUFFER;
	if(minimum < 1)
		minimum = 1;
	for(;;)
	{
		if(setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &requested, sizeof(requested)) == 0)
			break;
		if(requested <= minimum)
		{
			if(log)
				CHIAKI_LOGE(log, "SO_RCVBUF refused down to %d bytes", requested);
			return -1;
		}
		requested = requested / 2 > minimum ? requested / 2 : minimum;
	}
	int actual = 0;
	socklen_t size = sizeof(actual);
	if(getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &actual, &size) != 0)
		actual = requested;
	if(log)
		CHIAKI_LOGI(log, "Socket receive buffer: asked %d, accepted %d, socket reports %d bytes",
			OUNCE_RECEIVE_BUFFER, requested, actual);
	return actual > 0 ? actual : requested;
}

// A wait shorter than this found data already queued.
#define OUNCE_EMPTY_WAIT_US 1000

static _Atomic uint64_t backlog_since_us; // 0: the socket was empty at the last wait
static _Atomic uint64_t congestion_signals, fec_skipped, reports_coalesced;

void ounce_note_receive_wait(uint64_t start_us, uint64_t end_us)
{
	if(end_us - start_us >= OUNCE_EMPTY_WAIT_US)
		atomic_store_explicit(&backlog_since_us, 0, memory_order_relaxed);
	else if(!atomic_load_explicit(&backlog_since_us, memory_order_relaxed))
		atomic_store_explicit(&backlog_since_us, start_us ? start_us : 1, memory_order_relaxed);
}

int ounce_receive_backlog_ms(void)
{
	uint64_t since = atomic_load_explicit(&backlog_since_us, memory_order_relaxed);
	if(!since)
		return 0;
	uint64_t now = chiaki_time_now_monotonic_us();
	return now > since ? (int)((now - since) / 1000) : 0;
}

void ounce_note_congestion_signal(void)
{
	atomic_fetch_add_explicit(&congestion_signals, 1, memory_order_relaxed);
}

ChiakiBitstream *ounce_fec_skip_bitstream;

// While the receiver waits for an IDR it drops P-frames anyway, so running FEC
// on a lossy frame whose first slice is P only delays recovery. Unit 0 holds the
// first slice; if it is missing or doesn't parse, FEC runs as upstream.
int ounce_frame_skip_fec(ChiakiFrameProcessor *fp)
{
	if(!ounce_fec_skip_bitstream || !fp->unit_slots_size)
		return 0;
	size_t size = fp->unit_slots[0].data_size;
	if(size <= 2)
		return 0;
	ChiakiBitstreamSlice slice;
	if(!chiaki_bitstream_slice(ounce_fec_skip_bitstream, fp->frame_buf + 2, (unsigned)(size - 2), &slice)
		|| slice.slice_type != CHIAKI_BITSTREAM_SLICE_P)
		return 0;
	atomic_fetch_add_explicit(&fec_skipped, 1, memory_order_relaxed);
	return 1;
}

// Called only from the Takion thread.
int ounce_corrupt_report_due(uint16_t start, uint64_t now_ms)
{
	static int sent;
	static uint16_t last_start;
	static uint64_t last_ms;
	if(sent && start == last_start && now_ms - last_ms < 100)
	{
		atomic_fetch_add_explicit(&reports_coalesced, 1, memory_order_relaxed);
		return 0;
	}
	sent = 1;
	last_start = start;
	last_ms = now_ms;
	return 1;
}

OunceCoreStats ounce_core_stats(void)
{
	OunceCoreStats stats = {
		.congestion_signals = atomic_load_explicit(&congestion_signals, memory_order_relaxed),
		.fec_skipped = atomic_load_explicit(&fec_skipped, memory_order_relaxed),
		.reports_coalesced = atomic_load_explicit(&reports_coalesced, memory_order_relaxed),
		.backlog_ms = ounce_receive_backlog_ms(),
	};
	return stats;
}

void ounce_core_reset_stats(void)
{
	atomic_store(&backlog_since_us, 0);
	atomic_store(&congestion_signals, 0);
	atomic_store(&fec_skipped, 0);
	atomic_store(&reports_coalesced, 0);
}
