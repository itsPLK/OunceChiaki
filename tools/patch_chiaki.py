#!/usr/bin/env python3
"""Write patched copies of chiaki-ng sources into the build tree.

The submodule in third_party/chiaki-ng stays untouched. Every patch below must
match exactly once; a chiaki-ng update that moves a marker stops the build
instead of silently dropping a patch. The patches carry over CivChiaki's
streaming behavior (see docs/STREAMING.md); src/core/ holds the code they call.

Usage: patch_chiaki.py <chiaki-ng lib dir> <output dir>
"""
from pathlib import Path
import sys


def replace(text, old, new, name, count=1):
    found = text.count(old)
    if found != count:
        sys.exit(f"patch_chiaki: {name}: expected {count} match(es), found {found}")
    return text.replace(old, new)


def prepend(text, header):
    return header + text


def patch_takion(text):
    # Up to 1 MiB of socket storage absorbs bursts while the app is busy. The
    # advertised reliable window (a_rwnd) keeps upstream's value.
    # Upstream fails the session when setsockopt is refused; the helper steps
    # down until a size is accepted and logs what the socket got.
    rcvbuf = ("\t\tconst int rcvbuf_val = takion->a_rwnd;\n"
              "\t\tint r = setsockopt(takion->sock, SOL_SOCKET, SO_RCVBUF, "
              "(const CHIAKI_SOCKET_BUF_TYPE)&rcvbuf_val, sizeof(rcvbuf_val));")
    text = replace(text, rcvbuf,
                   "\t\tint r = ounce_set_receive_buffer(takion->sock, takion->a_rwnd, takion->log) > 0 ? 0 : -1;",
                   "takion receive buffer", count=2)
    # ACKs may carry duplicate TSNs after the gap blocks; upstream rejects them.
    text = replace(text, "if(buf_size != 0xc)", "if(buf_size < 0xc)", "takion ack header")
    text = text.replace("ack with size %zx != %#x", "ack with size %zx < %#x")
    text = replace(text, "if(buf_size != gap_ack_blocks_count * 4 + 0xc)",
                   "if(buf_size != ((size_t)gap_ack_blocks_count + dup_tsns_count) * 4 + 0xc)",
                   "takion ack size")
    # Receive backlog: time each wait for data. A wait that blocks means the
    # socket was empty; no blocking wait for 250 ms means a standing queue
    # (src/core/ounce_core.c, read by the congestion patch).
    select_call = ("\tChiakiErrorCode err = chiaki_stop_pipe_select_single(&takion->stop_pipe, "
                   "takion->sock, false, timeout_ms);")
    text = replace(text, select_call,
                   "\tuint64_t ounce_wait_start = chiaki_time_now_monotonic_us();\n" + select_call +
                   "\n\tounce_note_receive_wait(ounce_wait_start, chiaki_time_now_monotonic_us());",
                   "takion receive wait")
    return prepend(text, "#include <chiaki/time.h>\n#include \"ounce_core.h\"\n")


def patch_congestion(text):
    # Delay-based congestion signal. The 1 MiB receive storage absorbs overload
    # without loss, so the console would never lower its bitrate and the queue
    # would become input latency. While the backlog lasts >= 250 ms, report
    # loss at packet_loss_max so the console backs off. Real loss above that is
    # still clamped as upstream.
    report = "\t\tpacket.received = (uint16_t)received;"
    text = replace(text, report,
                   "\t\tif(total > 0 && ounce_receive_backlog_ms() >= 250)\n"
                   "\t\t{\n"
                   "\t\t\tuint64_t signal = (uint64_t)(total * control->packet_loss_max);\n"
                   "\t\t\tif(lost < signal)\n"
                   "\t\t\t{\n"
                   "\t\t\t\tlost = signal;\n"
                   "\t\t\t\treceived = total - lost;\n"
                   "\t\t\t\tounce_note_congestion_signal();\n"
                   "\t\t\t}\n"
                   "\t\t}\n" + report,
                   "congestion report")
    return prepend(text, "#include \"ounce_core.h\"\n")


def patch_stream_connection(text):
    # Corrupt-frame ranges are cumulative, so a report for the same start
    # within 100 ms is covered by the next one; don't flood the console.
    corrupt = ('\tCHIAKI_LOGW(stream_connection->log, "StreamConnection reporting corrupt frame(s) '
               'from %u to %u", (unsigned int)start, (unsigned int)end);')
    text = replace(text, corrupt,
                   "\tif(!ounce_corrupt_report_due(start, chiaki_time_now_monotonic_ms()))\n"
                   "\t\treturn CHIAKI_ERR_SUCCESS;\n" + corrupt,
                   "corrupt frame report")
    return prepend(text, "#include <chiaki/time.h>\n#include \"ounce_core.h\"\n")


def patch_frame_processor(text):
    # src/core/ounce_core.c reads the unit size through this private struct.
    if text.count("struct chiaki_frame_unit_t\n{\n\tsize_t data_size;\n};") != 1:
        sys.exit("patch_chiaki: frame unit layout changed; update ounce_frame_skip_fec")
    # While waiting for an IDR the receiver drops P-frames anyway: skip FEC.
    fec = "\t\tChiakiErrorCode err = chiaki_frame_processor_fec(frame_processor);"
    text = replace(text, fec,
                   "\t\tChiakiErrorCode err = ounce_frame_skip_fec(frame_processor) "
                   "? CHIAKI_ERR_FEC_FAILED : chiaki_frame_processor_fec(frame_processor);",
                   "frame processor fec")
    return prepend(text, "#include \"ounce_core.h\"\n")


def patch_video_receiver(text):
    flush = ("\tChiakiFrameProcessorFlushResult flush_result = chiaki_frame_processor_flush("
             "&video_receiver->frame_processor, &frame, &frame_size);")
    text = replace(text, flush,
                   "\tounce_fec_skip_bitstream = chiaki_video_receiver_get_waiting_for_idr(video_receiver) "
                   "? &video_receiver->bitstream : NULL;\n" + flush +
                   "\n\tounce_fec_skip_bitstream = NULL;",
                   "video receiver flush")
    return prepend(text, "#include \"ounce_core.h\"\n")


def patch_common(text):
    # aia.h needs curl (the internet path); chiaki_aia_init is a stub in
    # src/core/lan_only.c.
    return replace(text, '#include "aia.h"', "ChiakiErrorCode chiaki_aia_init(void);", "common aia include")


def patch_thread(text):
    # get_thread_limit() reads the process resource limit with raw SVCs; the
    # app is killed when it calls them under kaiHEN. pthread_create already
    # fails cleanly when no thread is left.
    return replace(text, "#ifdef __SWITCH__\n\tif(get_thread_limit() <= 1)\n\t\treturn CHIAKI_ERR_THREAD;\n#endif\n",
                   "", "thread limit check")


PATCHES = {
    "common.c": patch_common,
    "thread.c": patch_thread,
    "takion.c": patch_takion,
    "congestioncontrol.c": patch_congestion,
    "streamconnection.c": patch_stream_connection,
    "frameprocessor.c": patch_frame_processor,
    "videoreceiver.c": patch_video_receiver,
}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    lib, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for name, patch in PATCHES.items():
        source = (lib / "src" / name).read_text()
        target = out / name
        patched = patch(source)
        if not target.exists() or target.read_text() != patched:
            target.write_text(patched)


if __name__ == "__main__":
    main()
