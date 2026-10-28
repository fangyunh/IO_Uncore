# Unit-test image: the measurement build image + CUnit (SPDK --enable-unit-tests).
# Built on CPUs 8-15 by build_spdk_host.sh for ut-<name> variants only; never used for snapshots,
# so the measurement image (and its gcc) is not rebuilt. CUnit is pinned to the jammy version; the ut fingerprint
# records `dpkg-query -W libcunit1-dev` and the image id, which the ut runner checks.
FROM realcore-spdk-build:22.04
RUN apt-get update && apt-get install -y --no-install-recommends libcunit1-dev=2.1-3-dfsg-2.4 \
    && rm -rf /var/lib/apt/lists/* && test -f /usr/include/CUnit/CUnit.h
