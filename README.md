# IO Uncore: offloading NVMe queue handling from host cores

User-space storage stacks such as SPDK let one CPU core drive millions of I/Os per
second. At that rate a large share of every I/O is spent on NVMe protocol work rather
than on the application: building submission entries, ringing doorbells, and polling
and reaping completions. As SSDs keep getting faster, this per-I/O protocol cost on the
host core, not the device, increasingly limits how much I/O a core can sustain.

This project studies the **I/O Assistant Uncore (IAU)**, an on-die hardware block that
sits next to the CPU cores and takes over NVMe queue handling. Host software hands I/O
requests to the IAU and consumes completions in a form that is cheap to process; the IAU
manages the submission and completion queues and the doorbell traffic towards the SSD.
The goal is to free host cycles per I/O so that each core can drive more I/O, or spend
more of its time on the application.

## Research approach

- **Hardware design.** The IAU is designed at the RTL level and evaluated for area,
  power and timing in a predictive 7 nm process. The RTL is not part of this repository.
- **Real-core evaluation.** Instead of a simulator, the host side runs on real
  out-of-order server cores. An SPDK fork runs a software NVMe controller over
  **vfio-user** on dedicated device cores, with a software model of the IAU ported into
  it. The host runs the stock SPDK NVMe driver path (`spdk_nvme_perf`), either as the
  vanilla SPDK baseline or with the IAU-enabled host path, so the code under test is the
  real driver code on real cores.
- **Workloads.** Random 4 KiB reads and a replayed I/O trace of a billion-scale
  approximate nearest-neighbour vector search (DiskANN on BigANN), with read, write and
  mixed variants.

The work is in progress. Measurement results and the hardware design are intentionally
not published here.

## Repository contents

```
spdk/                 SPDK fork: IAU-enabled host NVMe path, perf app, vfio-user target with the IAU engine
docker/realcore/      Ubuntu 22.04 build containers
scripts/image/        build_spdk_host.sh: builds the fork into .build/spdk-host-<variant>/
scripts/realcore/     measurement harness: target start/stop, per-point runner, resumable campaigns
  inv/                experiment generator, overlay build variants, evidence checks
  next/               experiment definitions (arms = configurations, cells = workloads and core placement)
artifacts/bigann/     DiskANN/BigANN I/O traces replayed by the harness (read, write, 50/50 mix)
docs/CLOUDLAB.md      experiments planned on CloudLab and the hardware they need
```

## Build and run

Requires Docker (for the build container), Python 3 and a Linux host.

```bash
scripts/image/build_spdk_host.sh --variant lean          # measurement build, host probes compiled out

python3 scripts/realcore/inv/gen_inv_points.py \
    --arms scripts/realcore/next/arms/<arms>.csv \
    --cells scripts/realcore/next/cells/<cells>.csv \
    --repeats 5 --prefix <tag> > points.csv
scripts/realcore/campaign.sh points.csv <campaign>        # resumable; writes results/realcore/<campaign>/
python3 scripts/realcore/inv/check_l.py results/realcore/<campaign>
scripts/realcore/stop_target.sh                           # stop a target left running
```

Cell files pin application and device threads to specific CPUs; adapt `app_cpus`,
`dev_cpus` and `engine_cpus` to the topology of the machine.

## License

Apache 2.0 (see `LICENSE`). The SPDK fork under `spdk/` keeps SPDK's own BSD-3-Clause
license and the licenses of its bundled components.
