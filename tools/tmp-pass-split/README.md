# TEMP pass split trace

    python3 segagg.py eden_log.txt          # segments by end cause, small and empty ones
    python3 chains.py eden_log.txt segagg.py  # host vs guest passes, self-made splits

Frames counted: timed_ms > 20 and more than 50 segments (skips loading frames).
A pass whose next pass has the same framebuffer is a split the renderer made.
