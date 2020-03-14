FROM debian:stretch
#FROM continuumio/anaconda3
MAINTAINER Evangelos Karageorgos <karageorgos@pitt.edu>

# --------------- ensuring that the debian version is Stretch ---------------------


ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

RUN apt-get update --fix-missing && apt-get install -y wget bzip2 ca-certificates \
    libglib2.0-0 libxext6 libsm6 libxrender1 \
    git mercurial subversion

RUN apt-get install -y curl grep sed dpkg && \
    TINI_VERSION=`curl https://github.com/krallin/tini/releases/latest | grep -o "/v.*\"" | sed 's:^..\(.*\).$:\1:'` && \
    curl -L "https://github.com/krallin/tini/releases/download/v${TINI_VERSION}/tini_${TINI_VERSION}.deb" > tini.deb && \
    dpkg -i tini.deb && \
    rm tini.deb && \
    apt-get clean

# ----------------- Build essentials ---------------
RUN apt-get update
RUN apt-get -y install nano
RUN apt-get -y install build-essential

    
RUN mkdir /temp
ENV PATH /temp:$PATH
RUN { \
		echo "#!/bin/bash"; \
		echo "ip a | grep inet.*eth0 | awk '{print \$2}' | awk -F'/' '{print \$1}'"; \
    } | tee "/temp/myip"

RUN chmod +x /temp/myip

RUN { \
		echo "#!/bin/bash"; \
		echo "cd /home"; \
		echo "bash"; \
    } | tee "/temp/init.sh"

RUN chmod +x /temp/init.sh

ENTRYPOINT [ "/usr/bin/tini", "--" ]
CMD [ "/temp/init.sh" ]


# build:
#    docker network create -d bridge routed-net
#    docker build -t routed .

# windows run:
#    docker run -it --mount type=bind,source="%cd%",target=/home --network routed-net routed

# linux run:
#    docker run -it --mount type=bind,source="$PWD",target=/home --network routed-net routed

EXPOSE 80 443
