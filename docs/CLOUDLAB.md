# Planned experiments on CloudLab

Our current testbed is a shared server without root access. That rules out hugepages,
IOMMU-backed device assignment and direct access to NVMe SSDs, so today the SSD side is
always the emulated vfio-user controller. Bare-metal CloudLab nodes remove these limits.

## Experiments

1. **Real-drive anchor.** Run the vanilla SPDK baseline and the IAU-enabled host path
   against real NVMe SSDs bound to `vfio-pci`, to anchor the emulated-controller
   measurements to real devices, including the real cost of MMIO doorbell writes.
2. **Hugepage-backed memory.** Repeat the vfio-user campaigns with hugepage memory
   instead of the memfd-backed `--no-huge` mode our testbed forces, and check that the
   per-I/O host cost does not depend on the memory backing.
3. **Other CPU generations and vendors.** Repeat the per-core efficiency measurements on
   recent Intel Xeon and AMD EPYC nodes to test whether the host-side savings carry over
   across microarchitectures.
4. **Higher core counts.** Scale the multi-core runs beyond what our current host can
   supply, with enough cores per socket to host both the application cores and the
   emulated device cores on one socket.

## Hardware needed

- Bare-metal nodes with root access, IOMMU enabled and hugepages configurable.
- One or more local NVMe SSDs per node (experiment 1).
- At least 16 physical cores per socket; Intel and AMD node types (experiments 3 and 4).
- Docker for the build container.

## Procedure

Experiments 2 to 4 use the harness in `scripts/realcore/` as described in the top-level
README. Experiment 1 uses the same SPDK build with `spdk_nvme_perf` on a PCIe transport
(`trtype:PCIe`) after the SSD is bound with SPDK's `scripts/setup.sh`.
