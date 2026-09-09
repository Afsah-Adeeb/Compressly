# Builds the C++ compressor and serves the Python demo around it.
#
# Two stages: the first has the full toolchain (cmake, ninja, g++) and produces
# build/libcmpr.so; the second is a slim Python image that only needs that one
# shared library plus the service/ sources -- the toolchain earns nothing at
# runtime and would just bloat the image.
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tools ./tools
COPY tests ./tests

# Only the shared library is needed to serve requests -- skip the CLI, sweep and
# test targets so the container build doesn't pay for work the service never uses.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target cmpr_c

FROM python:3.11-slim AS runtime

WORKDIR /app
COPY service ./service
COPY --from=build /src/build/libcmpr.so ./build/libcmpr.so

# Matches service/cmpr.py's _default_library_path(): <repo root>/build/libcmpr.so,
# and __file__.resolve().parent.parent from service/server.py is /app here.
EXPOSE 8080

# Render (and most PaaS hosts) inject $PORT; default to 8080 for local `docker run`.
CMD ["sh", "-c", "python service/server.py --host 0.0.0.0 --port ${PORT:-8080}"]
