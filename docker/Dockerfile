# Pinned ESP-IDF toolchain for ImageJockey, T-Dongle-S3 variable-media-drive firmware.
# Espressif publishes a ready-built image with the full toolchain (compilers,
# idf.py, Python venv, TinyUSB submodule, etc.) already installed — we just
# pin the exact version so builds are reproducible across machines.
FROM espressif/idf:v5.5.5

ENV IDF_TARGET=esp32s3

WORKDIR /project

# The project directory is bind-mounted from the host (WSL2/Windows), so its
# file ownership doesn't match the container's user -- git refuses to run
# inside it ("detected dubious ownership") without this. Needed for
# CMakeLists.txt's `git describe` call that sets FIRMWARE_VERSION.
RUN git config --global --add safe.directory /project
