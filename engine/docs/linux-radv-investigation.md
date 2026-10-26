# RADV indirect-dispatch hang investigation

Reported with v0.9.37 beta 9 (`lantern-fps93`) on an RX 7900 XT (20 GiB),
Ryzen 9 9900X, Mesa/RADV 26.2.5, and Linux 7.2.11.

Six session logs stopped on indirect compute shaders `0000000042f2a521` or
`000000002da7fe60`, with X counts between 2.28 billion and 4.26 billion. One
recorded 2,811,384,408 x 1 x 1 groups. The kernel attributed the graphics-ring
timeouts to `bb-probe`, and ring recovery succeeded. Reported VRAM use before
that crash was about 4 GiB of 20 GiB.

The existing safeguard uses `maxComputeWorkGroupCount` directly. This RADV
device advertises 4,294,967,295 x 65,535 x 65,535, so the reported X values pass
unchanged. The proposed safety bounds also stop at 65,535 per dimension,
retaining smaller device limits and the existing `BB_INDIRECT_CLAMP=0` opt-out.
The breadcrumb report still captures the original guest counts, rather than
the copied dispatch arguments.

This is an experimental hang workaround. The producer or synchronization bug
behind the incorrect counts remains undiagnosed. Capping incorrect work may
change effects rendering, and a per-axis bound is not a total-work bound.

## Validation and limits

The GPU regression test shares the production limit helper. It checks the
recorded large X count, large Y/Z counts, normal counts, and a known-safe
one-workgroup indirect execution. Adversarial counts are inspected without
ever dispatching them. Separate result slots use at least 16 bytes, including
on RADV devices with four-byte storage-buffer alignment.

The combined source previously passed a full Linux build and five runtime
test programs. The GPU regression test passed on RX 7900 XT/RADV and Lavapipe
with Vulkan validation enabled. These checks establish build and isolated
clamp behavior, not prevention of the original gameplay hang.

The relevant renderer path is unchanged in v0.9.37 (`lantern-fps94`). The
reporter is testing that release with its original GPU library; the local
workaround is currently off and the original crash has not been confirmed
there. No current-release crash prevention or lighting/effects fix is claimed.
Linux online startup is independent and is tracked in PR #74.
