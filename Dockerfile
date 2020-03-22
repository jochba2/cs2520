FROM gcc:9
COPY src /src
COPY topo /topo
WORKDIR /src
RUN make
EXPOSE 5678