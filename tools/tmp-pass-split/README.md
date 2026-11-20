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
