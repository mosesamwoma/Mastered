FROM debian:bookworm-slim AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DMASTERED_BUILD_SHARED=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    && cmake --build build --target mastered_cli -j2

FROM python:3.12-slim-bookworm
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PORT=10000 \
    APP_ENV=production \
    WORK_DIR=/data
RUN apt-get update \
    && apt-get -y upgrade \
    && apt-get install -y --no-install-recommends ffmpeg util-linux \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 --shell /usr/sbin/nologin app \
    && mkdir /data \
    && chown app:app /data
WORKDIR /app
COPY web/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY web/app ./app
COPY web/static ./static
COPY --from=build /src/build/mastered_cli /usr/local/bin/mastered_cli
USER app
EXPOSE 10000
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD ["python", "-c", "import os, urllib.request; urllib.request.urlopen('http://127.0.0.1:' + os.environ.get('PORT', '10000') + '/api/ready', timeout=3)"]
CMD ["sh", "-c", "exec uvicorn app.main:app --host 0.0.0.0 --port ${PORT} --workers 1 --timeout-keep-alive 30 --no-access-log --no-server-header"]
