FROM ubuntu:noble
RUN export DEBIAN_FRONTEND=noninteractive && \
    TZ=Etc/UTC && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    tzdata unzip jq python3 clang make cmake
RUN ln /usr/bin/clang /usr/bin/gcc
RUN mkdir /sysprog
RUN mkdir /sysprog/solution

WORKDIR /sysprog

ENV RESOURCES_DIR_MOUNT='/tmp/data'
ENV SOLUTION_MOUNT='/opt/client/input'
ENV RESULT_LOCATION='/opt/results/output.json'

CMD ["/tmp/data/allcups/test.sh"]
