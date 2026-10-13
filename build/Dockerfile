# Build from the artifact root:  docker build -f build/Dockerfile -t grace .
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git unzip curl ca-certificates \
        python3 python3-pip \
        # >>> FairSyn system dependencies go here <<<
    && rm -rf /var/lib/apt/lists/*

WORKDIR /artifact
COPY . /artifact

RUN pip3 install --no-cache-dir -r build/requirements.txt
RUN ./build/build_all.sh

CMD ["bash"]