# CS 2520 - Project 1 - Link State Routing Protocol
## Authors
* **Joe Baker** - jcb116@pitt.edu
* **Evangelos Karageorgos** - karageorgos@pitt.edu

## Contributions

Work Item | Student
----------|--------
Design Report | Joe
README/User Manual | Joe
Docker Deployment | Joe
CLI/UI Thread | Joe
Link Map, Djikstra's (table.hpp) | Joe
BeNeighbor, Alive, Cost Message Handlers | Joe
LSA Message Handlers | Joe
Logging | Joe
TCP Connections | Evangelos
Background Thread Infrastructure | Evangelos
Message/Packetization | Evangelos
Common Datastructures | Evangelos
Final Report | Evangelos/Joe


## Getting Started

These instructions will get you a copy of the project up and running on your local machine for development and testing purposes. See deployment for notes on how to deploy the project on a live system.

### Prerequisites

* This source
* docker-compose (we tested on a version that supports docker-compose.yml version 3)
* The gcc:9 docker image
* Tested on docker for Windows and Linux

### Installing and Starting

#### Docker

To run our project, you can use docker-compose. The default config will start three containers on the same subnet with a router on each container.

The first run might take up to a minute or two to set up if your docker install needs to download the gcc:9 image (it's quick on PittNet).

```
$ docker-compose -f ./docker-compose-small.yml build
$ docker-compose -f ./docker-compose-small.yml up -d
```

Notes:

* You may need to run as root depending on your docker setup.
* `-d` runs in detached mode so you can pick what router CLI you want to log into. Running without `-d` is undefined behavior.

To see router and then attach to a router CLI:

```
$ docker container ls
CONTAINER ID        IMAGE               COMMAND                  CREATED             STATUS              PORTS               NAMES
9591fdd3f210        cs2520_routed_20    "./routed 172.16.238…"   34 hours ago        Up 19 minutes       5678/tcp            cs2520_routed_20_1
25dd4102ea32        cs2520_routed_10    "./routed 172.16.238…"   34 hours ago        Up 19 minutes       5678/tcp            cs2520_routed_10_1
339a157d3191        cs2520_routed_30    "./routed 172.16.238…"   34 hours ago        Up 19 minutes       5678/tcp            cs2520_routed_30_1

$ docker container attach cs2520_routed_10_1
router> help
CLI Usage:
        help - Print this message
        exit - Shut down the router
        start - Start communicating with other routers
        scp - Send a file to another router
        set-version - Change the routing protocol version
        set-link - Add/Update/Delete a link on this router
        show-path - Use this router's table to calculatethe path to another router
        show-graph - Print the this node's current network graph
routed>
```

You can use the router menu to monitor messages sent/received on this node, send files, and make live changes to the router. Type `help <cmd>` for usage on specific commands.

Notes:

* Hit enter once after the attach command to get the CLI prompt up
* From inside the routed prompt, use `ctl-p ctl-q` to detach back to your shell

#### Linux

Deploying this on a standalone Linux system is supported but not recommended. You will need a modern gcc compiler. From the root of the project:

```
$ cd ./src
$ make
$ ./routed
```

See the usage output from `./routed` for usage.

## Customizing the configuration of routers

### Links

You can set the details of links for a router before it starts running but not after. The links can be created/updated/deleted on the CLI with `set-link`. Links can also be set in bulk by specifying `link_list=./path_to_link_file.txt` when starting `./routed`. The format of a link configuration file is one line per link, with each field separated by spaces:

```
<string:link_ID> <string:dest_IPv4_addr> <double:cost> <int,seconds:hello_update_interval> <int,seconds:cost_update_interval>
...
```

The `./topo` directory contains two sample network topologies, small and large. Small is a 3 vertex graph with various weights and is sufficient to demonstrate all features of our project. Large is a 6 vertex bipartite graph that demonstrates that the implementation works at larger scales.

### docker-compose.yml

We provide two sample compose files, `docker-compose-small.yml` and `docker-compose-large.yml` for easy deployment when testing. Each compose file corresponds to the sample topologies above. Running these files will create directories that the containers will use to write logs files out to and use to send files between routers.

The files can be customized if you want to change any parameters that go into the starting command for the router, but those changes shouldn't be necessary. The easiest way to add new routers to the deployment is to add an entry under the `services:` section and edit these parts:

* Change `XYZ` to the last digits of an IPv4 address for your router
* Update `volumes->source` for log and file hosting directories on your machine

```yaml
  routed_XYZ:
    build: .
    stdin_open: true
    tty: true
    command:
      - "./routed"
      - "172.16.238.XYZ"
      - "/files"
      - "link_list=/topo/large_XYZ.txt"
      - "corrupt_msgs=1"
      - "auto_start=1"
      - "log_file=/logs/XYZ.log"
    networks:
      router-net:
        ipv4_address: 172.16.238.XYZ
    volumes:
      - type: bind
        source: ./<path_from_root_of_project_on_your_pc_to_log_output_dir>
        target: /logs
      - type: bind
        source: ./<path_from_root_of_project_on_your_pc_to_file_hosting_dir>/XYZ
        target: /files
 ```
 
 Note: You shouldn't need to change the `networks:` section at all.

### Linux

You can set the router's IP address, directory for hosting files and custom `link_list` files, or start the program and setup links by hand on all nodes in your topology.
