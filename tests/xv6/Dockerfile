FROM ubuntu:22.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        qemu-system-arm \
        ipxe-qemu \
    && rm -rf /var/lib/apt/lists/*

COPY build/xv6_ncc/kernel/kernel /xv6/kernel
COPY build/xv6_ncc/fs.img        /xv6/fs.img

ENTRYPOINT ["qemu-system-aarch64", \
    "-cpu",     "cortex-a72", \
    "-machine", "virt,gic-version=3", \
    "-kernel",  "/xv6/kernel", \
    "-m",       "128M", \
    "-smp",     "4", \
    "-nographic", \
    "-drive",   "file=/xv6/fs.img,if=none,format=raw,id=x0", \
    "-device",  "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0"]
