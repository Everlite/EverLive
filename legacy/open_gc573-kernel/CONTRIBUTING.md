# Contributing

Contributions are welcome, whether they fix a bug, improve an existing path,
add hardware support, or clarify behavior that was previously undocumented.
This driver was reconstructed from undocumented hardware, so changes need to
preserve both normal kernel-driver quality and the hardware invariants recovered
during that work.

## Engineering approach

Follow the Linux kernel coding style and prefer established PCI, V4L2,
videobuf2, ALSA, I2C, regmap, workqueue, locking, and kbuild interfaces. Keep
ownership clear: transport code owns hardware access, chip code owns chip-local
state and register sequencing, and V4L2 or ALSA code owns its userspace-facing
contract. Avoid generic frameworks or private APIs when a small typed interface
will do.

Treat register order, masks, delays, bank selection, reset sequencing,
interrupt acknowledgement, worker context, and state lifetime as functional
behavior. Simplify them only when equivalence is understood. Use neutral names
for unknown fields, and distinguish direct evidence from inference in comments
and pull-request descriptions. Do not copy proprietary source, decompiler
output, binaries, or extracted vendor artifacts into the repository.

Keep state per device and make lifecycle paths complete. Probe failures must
unwind cleanly; stream stop, module removal, suspend, resume, disconnect, and
reconnect must not leave DMA, interrupts, workers, or userspace endpoints in an
inconsistent state. Optional facilities must remain optional: failure or absence
of an auxiliary feature must not prevent otherwise supported capture hardware
from working.

Preserve kernel API semantics as carefully as hardware behavior. Report the
signal, format, colorimetry, audio layout, and capabilities actually provided;
do not relabel or silently convert them without an implemented and understood
conversion. A device match or shared chip identity is not by itself proof that
all initialization, routing, EDID, or feature policy can be shared.

Source files are named `gc573-<subsystem>.c`. Symbols keep the `gc555_` prefix
of the driver this tree was ported from.

```text
gc573-<subsystem>.c
gc555_<subsystem>_<operation>()
struct gc555_<name>
enum gc555_<name>
GC555_<SUBSYSTEM>_<NAME>
```

Chip-local code may use its chip prefix. Keep functions and data `static` unless
they are part of a necessary cross-file contract, and put declarations with the
subsystem that owns them.

## Evidence and support

Explain what establishes the correctness of a hardware-facing change. Useful
evidence includes behavior observed on hardware, agreement with an existing
working implementation, cross-driver static analysis, register traces, and
tests that isolate the affected state transition. If evidence is incomplete,
say what remains unknown rather than presenting an inference as established
behavior.

Implementation and validation are separate claims. Code may be accepted for an
untested path when the static evidence is strong and its scope is contained, but
documentation must continue to identify it as untested. New support claims must
state the hardware, source mode, pixel or audio format, and lifecycle operations
actually exercised. Representative tests are usually preferable to claiming an
entire family of modes from one successful example.

Changes should fix discovered defects rather than merely documenting them.
Conversely, avoid broad rewrites of working paths unless they solve a concrete
problem or make a proven invariant clearer.

## Validation

At minimum, build the proposed result with warnings enabled:

```sh
make -C "/lib/modules/$(uname -r)/build" \
    M="$PWD" CONFIG_VIDEO_GC555=m W=1 modules
```

Run sparse when available:

```sh
make -C "/lib/modules/$(uname -r)/build" \
    M="$PWD" CONFIG_VIDEO_GC555=m C=1 CHECK=sparse modules
```

Run the kernel's `scripts/checkpatch.pl` over the submitted commits and address
applicable findings. `Signed-off-by` trailers are not required by this project.

Runtime testing should match the risk of the change. A local cleanup may need
only review and a warning-free build. Changes to format negotiation, DMA,
interrupts, EDID, audio, teardown, power management, or hotplug require focused
tests of the affected path and nearby regressions. V4L2 API changes should retain
zero-failure `v4l2-compliance` results unless the pull request explains an
intentional contract change. If the required hardware is unavailable, submit
the static evidence and mark runtime validation as pending.

## Submitting changes

Keep each pull request coherent and avoid mixing functional work with unrelated
cleanup, developer tooling, or documentation churn. Helper scripts should solve
a demonstrated repository-wide need more safely or reliably than the standard
kernel workflow.

Commit messages and the pull-request description should make review possible
without reconstructing the development session. Describe the problem, the
relevant invariant, the chosen behavior, the evidence behind it, and the tests
performed. Update the README when supported, tested, untested, or unsupported
behavior changes, but do not turn it into a development log.
