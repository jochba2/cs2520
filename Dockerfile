FROM gcc:9
COPY src /src
WORKDIR /src
RUN make
EXPOSE 5678