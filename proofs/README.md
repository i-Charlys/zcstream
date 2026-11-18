# Transport model (TLA+)

`ZcTransport.tla` models the zcrudp video transport as `zcrudp_stream` runs it:
- the sender: frames, NACK answers, ZCSY, 200 ms linger and the tail probe at 20/40/60 ms;
- the ordered receiver: gap slots, NACK 3 ms after a frame is first seen then every 10 ms, give-up after 100 ms of silence, ZCSY every 150 ms while out of sync.

TLC checks it exhaustively. Every placement of up to K lost datagrams (data or control) is tried, and so is every delivery order within the network latency.

```
JAVA=java sh check.sh          # full suite, about 6 minutes on 24 cores
JAVA=java sh check.sh quick    # the two delivery runs
sh run.sh <cfg>                # one configuration (mk.sh writes them)
sh run.sh <cfg> | python trace.py   # a counterexample, one line per step
```

## Model

- **Time** is discrete, 1 unit = 1 ms.
  - Both programs poll every millisecond, so their timed actions are urgent: the clock does not advance while one is due.
  - A datagram arrives or is lost within `Lat` = 1 ms of being sent.
- **Not modelled**, because none of it carries reliability:
  - FEC parity (it only completes frames earlier);
  - pacing (a frame leaves in one instant);
  - ZCAC acknowledgements and the rate control;
  - the 32 reception slots and 64 history frames (N stays far below both).
- **Keyframes:** a keyframe is taken to restore the whole picture.

## Results

| Run | Setting | Result |
| :--- | :--- | :--- |
| `deliver4` | 3 frames × 2 fragments, real timings, **any 4 losses** | Every frame delivered (1.8 M states). The exception: a frame lost whole before the first delivery, covered below. |
| `tail5` | Same, 5 losses | Fails as expected. The last frame loses both fragments **and** all 3 probes (F + 3 losses), so the receiver can't know it existed. |
| `safe6` | Same, 6 losses | Safety and termination hold (7.9 M states). |
| `burst4` | Gap window 2 instead of 8, 4 frames × 1 fragment | The burst case with the fix: safety and termination hold. |
| `nofix_head`, `nofix_burst` | Without the fix | `NoSilentCorruption` is violated (the bug below). |
| `resync4` | Hold 10 ms, ZCSY every 30 ms, 4 losses | A bad picture is always restored if the sender keeps sending frames for 25 ms after it went bad. |
| `resync4_tight` | Same, 24 ms window | Fails as expected. The ZCSY pair is lost, and its retry 30 ms later arrives after the last frame. The bound is therefore exact: about one ZCSY interval (150 ms in the real code) per lost pair. |

In every run without an expected violation, these hold:
- **`InOrder`:** frames leave strictly in order and only complete.
- **`Consistent`**.
- **`NoSilentCorruption`:** a bad picture always has a resync requested.
- **`Ends`:** the session always terminates, so there is neither deadlock nor livelock.

## Bug found and fixed

`zc_video_rx` can skip a frame without giving it up. `rx_note_gap` holds nothing for frames it doesn't know about in two cases:
- before the first delivery (`last_completed_frame_id == UINT32_MAX`);
- for a frame more than 8 ahead (`ahead > 8`).

The next frame is then delivered as soon as it is complete, and `frames_dropped` does not move.

`run_receiver` asked for a resync only on `frames_dropped`, so the decoder applied a delta onto a stale picture and never asked for a keyframe.

The minimal counterexamples from TLC:
- **At the start:** frame 0 is incomplete and its NACKs are lost, frame 1 is lost whole, frame 2 arrives. Frames 0 and 2 are delivered, frame 1 is skipped silently.
- **Mid-stream:** more than 8 whole frames are lost in a row. That's 133 ms at 60 Hz, but only 17 ms at 480 Hz.

**Fix** (`stream_live.c`): a non-keyframe whose id does not follow the last one delivered sets `need_sync`.

## Limits that remain

- **A frame lost whole before the first delivery** is never asked for. The picture is still correct: a first frame that is not a keyframe triggers the resync.
- **The last frame** is lost for good if all its fragments and the 3 probes are lost. Nothing follows it, so nothing can reveal it.
- **Under unbounded loss**, delivery can't be guaranteed with a 100 ms give-up. What holds for any K is: in order, no deadlock, termination, and every bad picture triggers a resync.
