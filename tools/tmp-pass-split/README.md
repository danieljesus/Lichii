# TEMP pass split trace

    python3 segagg.py eden_log.txt          # segments by end cause, small and empty ones
    python3 chains.py eden_log.txt segagg.py  # host vs guest passes, self-made splits

Frames counted: timed_ms > 20 and more than 50 segments (skips loading frames).
A pass whose next pass has the same framebuffer is a split the renderer made.
    python3 splits.py eden_log.txt          # GPU ms/frame, host passes and self-made splits per frame

TEMP switches (environment):
    EDEN_TMP_FORCE_SPLIT=k   end the active render pass before every k-th draw (same framebuffer reopens, no flush)
Log lines:
    "TEMP seg ..."          every DKB-timed segment of a sampled frame, with what ended its pass
    "TEMP frametimes n=300 p50= p95= p99= max="   interval between Composite calls, every 300 frames
    python3 tails.py eden_log.txt           # tails by kind; pass-less submissions by what submitted them
    EDEN_TMP_PASS_STAMP_AFTER_BARRIER=1      stamp each pass after EndRenderPass and its barriers (their cost moves into the pass)
    EDEN_TMP_BEGIN_STAMP_BOTTOM=1            stamp each submission start at BOTTOM_OF_PIPE instead of TOP_OF_PIPE
A tail line now has end=flush|finish at=<site that submitted it>.
    python3 gaps.py eden_log.txt [min_s]    # GPU span/busy/idle a frame; idle between submissions by what submitted (run with EDEN_TMP_BEGIN_STAMP_BOTTOM=1)

On wip/mac/tmp-acquire-trace (nx2 based) the force split switch is not included.
    gaps.py also prints the GPU thread's waits for the back by site, the 10 s acquire report and the TEMP acquire drain split (EDEN_NX2_GPU_PROFILE on)
