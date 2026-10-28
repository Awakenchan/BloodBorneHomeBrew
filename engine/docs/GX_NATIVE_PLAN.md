# Native rendering from GX

Lantern Engine's renderer today reads the game's PM4 command buffers and works out each draw from
the GPU registers and the descriptors in guest memory (the shadPS4-derived path). This plan moves
it up a level: the game draws through GX, FromSoftware's graphics layer, which is shaped like
Direct3D 11. Each draw, dispatch, clear and copy is taken from GX's own objects at the call, and the
command buffer only keeps the place of each in the stream.

Every address below is an image offset (the eboot loaded at 0, as in all of Lantern's code). Every
fact is checked against the 1.09 eboot by `tools/re/gx_facts.py`, which must print `0 failed`
before any of them is used.

## What GX is in the eboot

| What | Where | How we know |
|---|---|---|
| Draw | `0x21696c0` | writes Gnm's drawIndexAuto, flushes the output merger and the command buffer |
| DrawIndexed | `0x2169780` | writes drawIndexOffset |
| DrawInstanced | `0x2169840` | drawIndexAuto with an instance count |
| DrawIndexedInstanced | `0x2169900` | drawIndexOffset with an instance count |
| DrawInstancedIndirect | `0x21699d0` | drawIndirect, then the post-draw |
| DrawIndexedInstancedIndirect | `0x2169ab0` | drawIndexIndirect, then the post-draw |
| Dispatch, DispatchIndirect | `0x21695c0`, `0x2169640` | dispatch and dispatchIndirect |
| Commit (state only) | `0x2169ba0` | the output-merger flush and the post-draw with no draw packet |
| Output-merger flush | `0x26d2230` | called by every direct draw and the commit |
| Post-draw | `0x26cbd50` | called after the indirect draws and the commit |
| Command buffer flush | `0x26af700` | the end of every GX operation |
| FinishCommandList | `0x26d5850` → `0x216aca0` | a deferred context's work as a command list |
| ExecuteCommandList, ExecuteCommandLists | `0x21679a0` → `0x216a7d0`, `0x21679e0` → `0x216a980` | the immediate context runs the lists in order |
| Segment submit | `0x26b0b00` | a command buffer's segment to the GPU |
| Close, submit and flip | `0x26afb30` → `0x26b0ce0` → `0x26b0e30` | the frame's last segment with the flip |
| Present | `0x26d5dd0` → `0x26d6e50` | sync interval clamped to 4, the swap chain at context +0x33d0 |
| CreateBuffer | `0x2165c10` | a 0x58-byte holder per buffer |
| Upload heap pool | `0x26a9b50`, made by the device at `0x216b0f0` | 0x30-byte nodes in 0xff0-byte blocks: every renamed constant buffer, Map rename and ring allocation |
| Occlusion query writer | `0x1077660` | |
| Pipeline drain | `0x26b09e0` | a label, an end-of-pipe event and a wait |
| Resource-table reclaim | `0x26aa860` | |
| Flush wait | `0x11d7030` | |
| Constant engine writers | `0x11fd190`–`0x11fd3e0` | |
| Gnm packet writers | `0x1072ed0`–`0x108b610` | `gnm_names.txt` |
| YEBIS's D3D11-shaped context | vtable `0x530a380` | 133 methods: Direct3D 11's 115 in order, then YEBIS's own; slot 50, ClearRenderTargetView, is `0x2a41320` |
| YEBIS's Gnm draw index, draw auto, dispatch | `0x11fbc40`, `0x11fbb50`, `0x11fbd10` | |
| Post-processing entry points | `0xbb68f0` (bind framebuffer), `0xbb6980` (apply effects) | |
| The flip pass (Gnmx) | `0x21d5f80` | its constant update engine at `0x1092e80` and `0x1093150` |
| The frame timer | `0x2034770` | the one the frame-rate patches already change |

GX's context methods are not found through a vtable in the image: they are called directly, and all
nine from one function at `0x26ab6b0`, still to be read.

## Still to find, with our own tools

- The GX context's pending state: where the bound shaders, views, constant buffers, input layout,
  vertex and index buffers and the blend, depth-stencil and rasterizer states sit. Read from what
  the commit and the output-merger flush load.
- GX's device: its texture, view, shader, state and query creators, found from CreateBuffer's
  callers and the device at `0x216b0f0`.
- Texture and view holders: size, the resource's memory, format and dimensions.
- Scaleform's renderer and its draws.

## Stages

Each stage is behind a `LANTERN_*` switch, off by default, with the current path kept. Each is
checked under the Vulkan validation layer for every family (`tools/validate_pipelines.sh`), goes out
in a beta, and becomes the default only after AMD and older NVIDIA cards have run it.

1. **Trace** (`LANTERN_GX_TRACE`). Hooks on the nine context methods count each kind of call a
   frame, and the pending state is read at each one and checked against what the PM4 path finds
   for the same draw. This proves the state offsets before anything depends on them.
2. **Objects** (`LANTERN_GX_OBJECTS`). GX's creators are hooked: every buffer, texture, view,
   shader and state object gets an entry in our tables when GX makes it, with a generation, and
   leaves it when it is released.
3. **Draws from GX** (`LANTERN_GX_DRAWS`). At a GX draw its inputs are recorded from the objects
   and a token takes its place in the command buffer. The GPU command thread builds the draw from
   the record, without reading registers or descriptors. A draw the record does not cover takes the
   PM4 path, and the log counts those by reason.
4. **Buffers by handle**. GX's immutable vertex and index buffers live in video memory from their
   creation, and draws bind them by handle. The upload heap is bound in place.
5. **Textures by object**. A texture's image belongs to its GX resource and generation: made at
   creation, filled by GX's upload and copy calls and the shaders that write it, dropped at
   release. No more guessing by address and hash. YEBIS's textures come from its own creation calls.
6. **Compute, clears, copies, queries and drains as tokens** from GX's calls, YEBIS's from YEBIS's
   context.
7. **Completions on the GPU's timeline**. The labels GX waits on are written when the submission
   has run. The PM4 walk then only orders the tokens.
8. **The shader lifter**. GCN shaders become structured, typed SPIR-V beside the native compiler,
   compared with it shader by shader.
9. **The translator goes**. Once stages 3–7 are the default everywhere, the PM4 path, the SRT walker,
   the constant-engine emulation, the GPU-write tracking and the address-keyed texture cache are
   deleted.

## Rules

- Facts only from outside sources: functions, addresses, what a call does. Each one is confirmed in
  the eboot by our own tools before it is used, and the code is written fresh.
- No code, file layout or text is taken from other projects.
