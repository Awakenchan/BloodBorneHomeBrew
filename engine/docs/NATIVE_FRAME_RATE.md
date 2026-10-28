# Native frame rate

The frame rate is the engine's (`gpu/shim/engine/frame_rate.cpp`), not byte lists the start applies before
boot the way shadPS4 applies patches. With `LANTERN_FRAMERATE` on (Engine switches, the default) and the
verified 1.09 executable, the start leaves the frame-rate lists out (`LANTERN_FRAMERATE_FROM_ENGINE=1`, Windows
`start/patches.cpp`, Linux `run.sh` and `scripts/patches.py`) and the engine writes the frame timer's mode, the
frame-time cave, the limiter, the steps and the sprint fix itself from `frame_rate_lists.inc` (each write
checked against the eboot's own bytes; a write another patch already changed is left to it), then runs
everything below. Off, the start writes the lists as before and the engine works on top of them.

## Why

0.9.37 wrote fixed per-frame values for the chosen rate. When the game did not hold that rate, game time
slowed by the same factor: a player at 120 FPS running 52-60 saw slow ragdolls and "anti-gravity" corpses,
one at 60 running 27-45 played in slow motion, frame generation at a 120 cap (the game near 60) slowed
stamina regeneration. The uncapped list kept 1/60 steps at any rate and the 90 list 1/60 at 90.
Windows and Linux also differed: only the Windows start rescaled the steps for 120 and 144.

## Sites

`tools/re/frame_rate_sites.py` builds `gpu/shim/engine/frame_rate_sites.inc` from the shipped 60, 90 and
uncapped lists and the 1.09 eboot (`--write`; without it, it checks the shipped table):

| Kind | Sites | Value at rate r |
|---|---|---|
| Step | 57 code immediates and 19 constants that every list sets to 1/60 (1/30 in the eboot) | 1/r |
| HighStep | `0xF383CA`, `0x4D29170` | 1/r above 60, else the game's 1/30 |
| EffectStep | the 90 list's store at `0x1CBDB42` | 1.125/r |
| CountSixth, CountThird | `0x1929676`, `0x192C405` | r/6, r/3 frames |
| Three, NegThree | `0x4CF6420` (four), `0x4D27FD4`, `0x4D27FE0` | ±3/r |
| EaseDistance, EaseChase | `0x4D25E68`, `0x4D25E94` | 1 - 0.9^(30/r), 1 - (29/30)^(30/r) |

Checked readers of what changes every frame (1.09 eboot): the camera eases at `0x183AF8C` and `0x183E2B2`
(follow camera `0x183AC60`, `x += (target - x) * w`); the ±3/r pair at `0x1D084F5`-`0x1D0854C` (a fade at
`[rbx+0x578]` stepped and clamped); the 3/r vector at `0x192C57D` (`(new - old) * k` into `[rbx+0xD0]`); the five
`vmaxss` floors at `0x12E596A`-`0x12F6992` (a duration of at least one frame). The step constants read only by
one-time setup (`0x1935C56`, `0x19369B4`, the eight parameter registrations from `0x1C1FF25`) take the target's
value. Four step constants whose readers are not proven per frame (`0x4C88688`, `0x4D26C9C`, `0x4D270E4`,
`0x4D28418`: TargetStep) stay at the target, as the lists set them.

Code immediates are checked by the four bytes before them, constants by a plausible range for their
kind; a site that holds neither is left alone and counted in the log.

## At run time

- Installed with the other engine features when a list's frame-time cave (`0x2418E3D`) is in the image.
  The game's own 30 FPS (no list) is left alone.
- Every value is set for the target: `BB_FPS` (60, 90, 120, 144) or the uncapped limit (240 without one).
- Each game frame (the frame-time manager hook, `0x2434770`), the constants follow the frame time the list's
  cave measures (`0x575A908`), clamped to [1/target, 1/30].
- The code immediates and counts follow the rate the game holds: up after 30 frames, down after 120, never
  down on a 30 FPS stall (loading screens). They are written through the hook layer's `WriteCode`.
- Engine switches: `LANTERN_FRAMERATE` (native frame rate) and `LANTERN_FRAMERATE_FOLLOW` (steps follow the rate
  held; off keeps every value at the target), both on.

## Live steps (tried, taken out)

42 of the code immediates are an update's step: `mov dword [rbp+d], 1/30` into a struct whose first field is the
step-context vtable (`0x56EFD40`, at `[rbp+d-8]`), passed by `lea rsi, [rbp+d-8]` to the call that follows
(`frame_rate_sites.py` checks the pattern: ContextStep). Stubs that wrote the eased frame time into them every
frame were tried on 2026-10-28: with the frame rate swinging (30-120 FPS) the player shook on elevators, as the main
step takes the measured time. They follow the held rate like the other code immediates, as in the build where
elevators were right. Making them live again needs the main step's own (measured) time, checked first.

## The main step

The task step `0x2418D20` (after the frame-time manager `0x2434770`, once a frame) builds the main step
`{0x56EFD40, dt}` at `[rbp-0x40]` and passes it at `0x2418E6B`; the frame-time cave stores the measured frame
time there (clamped), while the other systems read the eased time at `0x575A908`. Giving the main step the eased
time as well (with a 6% snap to the target) was tried on 2026-10-28: at 47-57 FPS the player shook on elevators,
so the main step keeps the measured time, as in the build where elevators were right.

## The engine's writes match the start's

`tests/test_frame_rate_native.py` builds the image both ways at 60, 120 and 144 FPS (the start's lists with its
120/144 rescaling and camera eases, and the engine's own writes and values) and requires every byte to match.
It caught the one difference that made players shake on elevators: the step at `0x1BF9B74` (the uncapped list
replaces its instruction, so a table built from all three lists missed it) stayed 1/60 at 120 FPS. The table is
built from the 60 and 90 lists; that step stays at the target, as the start set it.

## Next stages

1. The frame timer, the limiter and the 30 FPS rates (`STEP_THIRTY_*`) from the engine, changeable while
   playing (the overlay's frame rate).
2. The XML frame-rate lists retired: the start no longer writes them.
3. Charged attacks, ladders, elevators and the Moonlight Greatsword's beam checked at 60, 90, 120 and uncapped
   against beta 9.
