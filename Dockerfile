# ============================================================
# TinyRPC Dockerfile
# 基于 Ubuntu 24.04 + g++-14 + CMake + Protobuf
# 使用阿里云镜像源（国内网络兼容）
# ============================================================

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 替换为阿里云镜像源
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.aliyun.com|g; s|http://security.ubuntu.com|http://mirrors.aliyun.com|g' \
        /etc/apt/sources.list.d/ubuntu.sources

# 安装构建依赖
RUN apt update \
    && apt install -y \
        g++ \
        cmake \
        make \
        protobuf-compiler \
        libprotobuf-dev \
    && apt clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# 构建项目
RUN mkdir build && cd build \
    && cmake .. && make -j$(nproc)

EXPOSE 8080
CMD ["./build/rpc"]
