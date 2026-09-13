# task-engine container image: a reproducible build of the CLI (D31).
#
# Two stages. The build stage carries the compiler, CMake and git, builds the
# project in Release, and runs the unit, concurrency and integration suites, so
# an image can only be produced if the tests pass inside it. The runtime stage
# carries the binary and nothing it does not already need.
#
# Both stages use the same base, pinned by digest. A tag can be repointed
# upstream; a digest cannot. Using one base for both stages is also what makes
# the runtime stage safe: the binary is linked against this glibc and libstdc++,
# and a smaller base from another distribution could carry older versions of
# either and fail to start it.
#
# The base is pinned; the apt packages installed on top resolve against the
# Ubuntu 24.04 archive at build time, so the build is reproducible in toolchain
# series (GCC 13, CMake 3.28) rather than bit-for-bit.
#
# Not a measurement environment. Benchmark numbers come from a Release build on
# the host, never from inside this image (ARCHITECTURE.md 8.5, D31).

# ---------------------------------------------------------------------------
# Build stage
# ---------------------------------------------------------------------------
FROM ubuntu:24.04@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254 AS build

# Toolchain only in this stage. git and ca-certificates are here because CMake
# fetches GoogleTest at configure time.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates cmake g++ git make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Stress tests are left to the host sanitizer runs: they exist to be judged by
# ThreadSanitizer, and repeating them here would only lengthen every image build.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure --timeout 600 -LE stress

# ---------------------------------------------------------------------------
# Runtime stage
# ---------------------------------------------------------------------------
FROM ubuntu:24.04@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254

# A dedicated unprivileged account. The engine needs no privileges, so it runs
# with none.
RUN useradd --system --no-create-home --shell /usr/sbin/nologin taskengine

COPY --from=build /src/build/task-engine /usr/local/bin/task-engine

USER taskengine

ENTRYPOINT ["/usr/local/bin/task-engine"]
CMD ["--help"]
