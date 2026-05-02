FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        zlib1g-dev \
        bison flex byacc \
        ca-certificates \
        git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /netbsd/src

CMD ["bash"]
