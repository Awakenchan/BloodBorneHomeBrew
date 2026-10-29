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

GX's context and device methods are not found through a vtable in the image: they are called
directly.

## GX's command lists and pending state (2026-10-29)

Found with the Ghidra project (`tools/re/ghidra.sh`, see `NATIVE_ENGINE_PLAN.md`) and checked by
`gx_facts.py` (162 checks, `0 failed`).

A deferred context records its calls as GX's own records, and the immediate context replays them
through a table of 36 handlers at `0x52b9fb8`. Each handler reads one record, calls the same context
method the game calls directly, keeps the value in the pending state and returns the record's size.
`0x26ab6b0` is handler 28 (Dispatch), not a dispatcher. So hooking the context methods covers
deferred work too: replay goes through them.

The pending state is the second argument of every context method (the first is the context):

| Handler | Method | Pending state |
|---|---|---|
| 0 | IASetInputLayout `0x2168c90` | `+0x0` |
| 1 | IASetPrimitiveTopology `0x2168ca0` | `+0x8` |
| 2 | IASetIndexBuffer `0x2168cb0` | `+0x10` buffer, `+0x18` format, `+0x1c` offset |
| 3 | IASetVertexBuffers `0x2168cc0` | `+0x20` buffers, `+0x120` strides, `+0x1a0` offsets (32 slots) |
| 4–27 | six shader stages, four setters each | per stage: 16 constant buffers, 128 shader resources, 14 samplers, the shader |
| 28–35 | Dispatch, Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, both indirect draws, Commit | |

The stages' blocks start at `+0x598`, `+0xa90`, `+0xf88`, `+0x1480`, `+0x1978` and `+0x1e70`, in
replay order. The last is the compute stage (it is set before Dispatch); which graphics stage is
which is still to be confirmed from the shader creators.

| Stage block | SetConstantBuffers | SetShaderResources | SetSamplers | SetShader |
|---|---|---|---|---|
| `+0x598` | `0x2168ce0` | `0x2168d00` | `0x2168d90` | `0x2168e00` |
| `+0xa90` | `0x2168e10` | `0x2168e30` | `0x2168ec0` | `0x2168f30` |
| `+0xf88` | `0x2168f40` | `0x2168f60` | `0x2168ff0` | `0x2169060` |
| `+0x1480` | `0x2169070` | `0x2169090` | `0x2169120` | `0x2169190` |
| `+0x1978` | `0x21692c0` | `0x21692e0` | `0x2169370` | `0x21693e0` |
| `+0x1e70` (compute) | `0x2169440` | `0x2169460` | `0x21694f0` | `0x21695b0` |

The output merger's state is read by its flush (`0x26d2230`): the blend state at `+0x4d0`, the
depth-stencil state at `+0x4d8`, eight render targets at `+0x4e0`, the depth view at `+0x520`, the
blend factor at `+0x568` and the stencil reference at `+0x584`. These setters are not in the replay
table, so a command list does not record them.

Every draw runs the same flushes before its packet: the output merger (`context+0xb558`), the
shader stages (`0x26bba30` on `context+0xb518`, with an "indexed" flag), `0x26d3370` on
`context+0xb528`, the command flush, and the resource bind `0x26bc2e0` on `context+0x50`; then the
Gnm writers on the command buffer at `context+0x10`, then the post-draw.

## GX's device creators

All take a Direct3D 11-shaped description. CreateBuffer's is D3D11's (byte width, usage 0–3,
bind flags, CPU access, misc flags, structure stride); usage 1 (immutable) needs the initial data.

| Creator | Address | Object |
|---|---|---|
| CreateBuffer | `0x2165c10` | 0x58 bytes, vtable `0x535f3a0` |
| CreateTexture2D | `0x2165f90` | description: width, height, mips, array size, format, samples, usage, bind, CPU, misc |
| CreateTexture3D | `0x2166300` | width, height, depth, mips, format, usage, bind, CPU, misc |
| CreateDepthStencilView | `0x2166610` | 0x60 bytes, vtable `0x535f420`; needs bind flag 0x40 |
| CreateRenderTargetView | `0x21667a0` | 0x50 bytes, vtable `0x535f440`; bind flag 0x20 |
| CreateShaderResourceView | `0x21669a0` | 0x50 bytes, vtable `0x535f460`; bind flag 0x8 |
| CreateUnorderedAccessView | `0x2166b60` | 0x50 bytes, vtable `0x535f480`; bind flag 0x80 |
| CreateGeometryShader | `0x2167030` | 0x150 bytes; its check `0x2173220` wants the `Shdr` magic at `+0x24` and type 3 at `+0x2c` |
| CreateGeometryShaderWithStreamOutput | `0x2167140` | 0x150 bytes, through the same check |
| Other shaders (five) | `0x2166d00` (0x128), `0x2166e10`, `0x2166f20`, `0x21672d0`, `0x21673e0` (0x120) | the binary is PSSL (`Shdr` at `+0x24`) |
| CreateDeferredContext | `0x21676c0` | a 0xdae0-byte context |

A view's resource keeps its dimension in a byte at `+0x38`, checked against the view's dimension.

## Still to find, with our own tools

- Which of the five other shader creators is which stage, and so which stage block is which. The
  cheapest proof is at run time in stage 1: log which creator made the shader each SetShader gets.
- The blend, depth-stencil, rasterizer and sampler state creators, and the rasterizer state and
  viewports in the pending state (between `+0x528` and `+0x598`).
- The texture holder: where the resource's memory, format and tiling mode sit.
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
