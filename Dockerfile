# Buduj na Macu (Apple Silicon):
#   docker build --platform linux/amd64 -t pb-rsvp-builder .
#
# Kompilacja aplikacji:
#   docker run --rm -it --platform linux/amd64 -v "$(pwd):/project" pb-rsvp-builder \
#     -c 'mkdir -p build && cd build && cmake -DCMAKE_TOOLCHAIN_FILE=/SDK/share/cmake/arm_conf.cmake .. && cmake --build .'
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG PB_DEVICE=verse-pro-color
ARG SDK_REPO_URL=https://github.com/pocketbook/SDK_6.3.0.git
ARG SDK_BRANCH=5.19
ARG SDK_RELEASE=6.8

ENV SDK_BASE=/SDK
ENV PB_DEPS=/pb-deps
# Biblioteki hosta potrzebne starym narzędziom z toolchaina PB (gcc 6.x)
ENV LD_LIBRARY_PATH=/SDK/usr/lib:/SDK/lib:/usr/lib/x86_64-linux-gnu

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    locales \
    ca-certificates \
    curl \
    git \
    build-essential \
    pkg-config \
    make \
    cmake \
    xz-utils \
    p7zip-full \
    python3 \
    libtinfo5 \
    libncurses5 \
    libgmp10 \
  && rm -rf /var/lib/apt/lists/* \
  # Stary GCC z SDK potrzebuje libmpfr.so.4 / libmpc.so.3 / libisl.so.15
  # (Ubuntu 22.04 ma nowsze wersje .so — dokładamy paczki z archiwum)
  && mkdir -p /tmp/oldlibs \
  && cd /tmp/oldlibs \
  && curl -L --fail -O http://archive.ubuntu.com/ubuntu/pool/main/m/mpfr4/libmpfr4_3.1.4-1_amd64.deb \
  && curl -L --fail -O http://archive.ubuntu.com/ubuntu/pool/main/m/mpclib3/libmpc3_1.1.0-1_amd64.deb \
  && curl -L --fail -O http://archive.ubuntu.com/ubuntu/pool/main/i/isl/libisl15_0.16.1-1_amd64.deb \
  && dpkg -i ./*.deb \
  && rm -rf /tmp/oldlibs \
  && ldconfig \
  && test -e /usr/lib/x86_64-linux-gnu/libmpfr.so.4 \
  && test -e /usr/lib/x86_64-linux-gnu/libmpc.so.3


RUN localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# SDK PocketBook
RUN set -eux; \
  case "${PB_DEVICE}" in \
    verse) \
      git clone --depth 1 --branch "${SDK_BRANCH}" "${SDK_REPO_URL}" /tmp/SDK_6.3.0; \
      mv /tmp/SDK_6.3.0/SDK-B288 "${SDK_BASE}"; \
      rm -rf /tmp/SDK_6.3.0; \
      ;; \
    verse-pro-color) \
      curl -L --fail --retry 5 --retry-delay 2 \
        -o /tmp/sdk.7z \
        "https://github.com/pocketbook/SDK_6.3.0/releases/download/${SDK_RELEASE}/SDK-B300-${SDK_RELEASE}.7z"; \
      7z x -y /tmp/sdk.7z -o/tmp/sdk-extract; \
      if [ -d /tmp/sdk-extract/SDK-B300 ]; then \
        mv /tmp/sdk-extract/SDK-B300 "${SDK_BASE}"; \
      else \
        mv /tmp/sdk-extract/* "${SDK_BASE}"; \
      fi; \
      rm -rf /tmp/sdk.7z /tmp/sdk-extract; \
      ;; \
    *) echo "PB_DEVICE musi być: verse | verse-pro-color" >&2; exit 1 ;; \
  esac; \
  test -x "${SDK_BASE}/bin/update_path.sh"; \
  "${SDK_BASE}/bin/update_path.sh"; \
  export PATH="${SDK_BASE}/usr/bin:${PATH}"; \
  export LD_LIBRARY_PATH="${SDK_BASE}/usr/lib:${SDK_BASE}/lib:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"; \
  # Wczesny test: kompilator musi się uruchomić i zrobić plik .o
  echo 'int x;' > /tmp/t.c; \
  if [ -x "${SDK_BASE}/usr/bin/arm-obreey-linux-gnueabi-gcc" ]; then \
    arm-obreey-linux-gnueabi-gcc --version; \
    arm-obreey-linux-gnueabi-gcc -O2 -fPIC --sysroot="${SDK_BASE}/usr/arm-obreey-linux-gnueabi/sysroot" -c /tmp/t.c -o /tmp/t.o; \
  else \
    arm-obreey-linux-gnueabi-clang --version; \
    arm-obreey-linux-gnueabi-clang -O2 -fPIC --sysroot="${SDK_BASE}/usr/arm-obreey-linux-gnueabi/sysroot" -c /tmp/t.c -o /tmp/t.o; \
  fi; \
  rm -f /tmp/t.c /tmp/t.o; \
  echo "SDK compiler OK"

# CMake x86_64
RUN curl -L --fail \
      https://github.com/Kitware/CMake/releases/download/v3.21.3/cmake-3.21.3-linux-x86_64.tar.gz \
    | tar xz --strip-components=1 -C /usr \
  && cmake --version

# Skrypt budowy libxml2/libzip
COPY docker/build-deps.sh /usr/local/bin/pb-build-deps.sh
RUN chmod +x /usr/local/bin/pb-build-deps.sh \
  && /usr/local/bin/pb-build-deps.sh \
  && rm -rf /tmp/pb-build

WORKDIR /project
VOLUME ["/project"]
ENV PATH="/SDK/usr/bin:${PATH}"
ENTRYPOINT ["bash"]
