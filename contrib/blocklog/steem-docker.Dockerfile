# steem-docker's dkr/Dockerfile (https://github.com/Someguy123/steem-docker) with the build arguments pointed at
# the block log compression fork. Nothing else is different - a node operator who already uses steem-docker can
# also keep his own Dockerfile and just pass the two --build-arg values below.
#
#   docker build -f contrib/blocklog/steem-docker.Dockerfile -t steem-zstd .
#   # or, with the stock steem-docker Dockerfile:
#   docker build --build-arg STEEM_SOURCE=<fork url> --build-arg steemd_version=<branch or tag> -t steem-zstd dkr/
#
# The image contains steemd and compress_block_log (both in /usr/local/bin).

FROM ubuntu:bionic

RUN apt-get update && \
    apt-get install -y gcc g++ cmake make libbz2-dev libdb++-dev libdb-dev && \
    apt-get install -y libssl-dev openssl libreadline-dev autoconf libtool git gdb liblz4-tool wget jq virtualenv libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev liblz4-dev libzstd-dev && \
    apt-get install -y autotools-dev build-essential libbz2-dev libicu-dev python-dev wget doxygen python3 python3-dev python3-pip libboost-all-dev curl && \
    apt-get clean -qy

RUN pip3 install jinja2

# Build arguments (--build-arg NAME=value). The source must be a branch or a tag.
ARG steemd_version=blocklog-zstd
ENV steemd_version ${steemd_version}

ARG STEEM_SOURCE="https://github.com/REPLACE-WITH-FORK/steem.git"
ENV STEEM_SOURCE ${STEEM_SOURCE}

ARG STEEM_STATIC_BUILD=ON
ENV STEEM_STATIC_BUILD ${STEEM_STATIC_BUILD}

ARG ENABLE_MIRA=ON
ENV ENABLE_MIRA ${ENABLE_MIRA}

ARG LOW_MEMORY_MODE=ON
ENV LOW_MEMORY_MODE ${LOW_MEMORY_MODE}

ARG CLEAR_VOTES=ON
ENV CLEAR_VOTES ${CLEAR_VOTES}

ARG SKIP_BY_TX_ID=ON
ENV SKIP_BY_TX_ID ${SKIP_BY_TX_ID}

RUN cd ~ && \
    echo " >>> Cloning tag/branch ${steemd_version} from repo: ${STEEM_SOURCE}" && \
    git clone ${STEEM_SOURCE} steem -b ${steemd_version} && \
    cd steem && \
    git submodule update --init --recursive && \
    cmake -DCMAKE_BUILD_TYPE=Release . \
        -DLOW_MEMORY_NODE=${LOW_MEMORY_MODE} \
        -DCLEAR_VOTES=${CLEAR_VOTES} \
        -DSTEEM_STATIC_BUILD=${STEEM_STATIC_BUILD} \
        -DSKIP_BY_TX_ID=${SKIP_BY_TX_ID} \
        -DENABLE_MIRA=${ENABLE_MIRA} && \
    make -j$(nproc) steemd compress_block_log cli_wallet && make install && rm -rf ~/steem

VOLUME /steem
WORKDIR /steem

# P2P (seed) port
EXPOSE 2001
# RPC ports
EXPOSE 5000
EXPOSE 8090
EXPOSE 8091

ARG STEEMD_BIN="/usr/local/bin/steemd"
ENV STEEMD_BIN ${STEEMD_BIN}

RUN echo "This container has been built with the following options:" >> /steem_build.txt && \
    echo "----" >> /steem_build.txt && \
    echo "Git Repository:              ${STEEM_SOURCE}" >> /steem_build.txt && \
    echo "Git version/commit:          ${steemd_version}\n----" >> /steem_build.txt && \
    echo "Default steemd executable:   ${STEEMD_BIN}\n---" >> /steem_build.txt && \
    echo "LOW_MEMORY_MODE=${LOW_MEMORY_MODE}\nSTEEM_STATIC_BUILD=${STEEM_STATIC_BUILD}" >> /steem_build.txt && \
    echo "SKIP_BY_TX_ID=${SKIP_BY_TX_ID}\nENABLE_MIRA=${ENABLE_MIRA}\nCLEAR_VOTES=${CLEAR_VOTES}" >> /steem_build.txt && \
    echo "----\nBuilt at: $(date)\n----" >> /steem_build.txt

CMD ["sh", "-c", "${STEEMD_BIN}"]
