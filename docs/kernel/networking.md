# Networking

A network stack turns the frames a network card receives into data a
program reads from a socket, and back. It is built in layers, each with
its own header on the front of the packet: Ethernet carries frames
between machines on one link, IPv4 carries packets between machines
anywhere, through routers, and UDP and TCP carry data between programs
on those machines. Each layer only reads its own header and hands the
rest up, or puts its header on and hands the packet down.

All of this is the kernel's own: no part of the stack depends on the
CPU or the machine. Only the network card's driver is a driver, and
like other drivers it is told its interrupt by its bus and installs its
handler with `irq_install()` (see "Devices: drivers and the platform" in
[The kernel and the machine](architecture.md)).

This document goes from the top down: sockets, as programs see them,
then the layers under them, then the network's thread, which runs the
stack, and the driver at the bottom.

## Sockets

A **socket** is an endpoint of communication, opened with `socket()`
and used through a file descriptor, so `read()`, `write()`, `poll()` and
`close()` work on it as on any file. What it talks to depends on its
*domain* and *type*:

| Domain     | Type          | What                                                   |
|------------|---------------|--------------------------------------------------------|
| `AF_UNIX`  | `SOCK_STREAM` | a byte stream between two processes on this machine, named by a file |
| `AF_INET`  | `SOCK_STREAM` | TCP: a reliable byte stream to another machine         |
| `AF_INET`  | `SOCK_DGRAM`  | UDP: datagrams, delivered at most once, in any order   |
| `AF_INET`  | `SOCK_DGRAM`, `IPPROTO_ICMP` | ping: ICMP echo requests and their replies, without being root |

The socket layer ([`net/socket.c`](../../kernel/net/socket.c),
[`include/net/socket.h`](../../kernel/include/net/socket.h)) is a table
of functions: a socket has a `struct sock_ops` (`bind`, `connect`,
`listen`, `accept`, `send`, `recv`, `getname`, `getsockopt`,
`setsockopt`, `ioctl`, `can_read`, `can_write`, `wait_queue`, `shut`,
`shutdown`), and the system calls call the socket's. `socket_create()`
picks the table by domain and type. A file that is a socket has `FILE_SOCKET` in its flags;
`read()` on it is `recv()`, `write()` is `send()`
([`fs/fops.c`](../../kernel/fs/fops.c)).

**Waiting.** A reader with nothing to read sleeps on its socket's wait
queue until data comes, and so does `poll()` or `select()` on it: the
socket's `wait_queue` names the queue a reader (or a writer) waits on,
and `can_read`/`can_write` say whether it would wait. A socket with
`O_NONBLOCK` returns `EAGAIN` instead. `SO_RCVTIMEO` and `SO_SNDTIMEO`
bound the wait.

### Unix sockets

`AF_UNIX` sockets ([`net/unix/unix.c`](../../kernel/net/unix/unix.c))
connect processes on the same machine, by a path in the filesystem:

- `bind()` makes a socket node at the path (`S_IFSOCK`, mode `0777` less
  the umask); `listen()` and `accept()` take connections to it.
- `connect()` looks the path up, and the listener's `accept()` takes the
  connection.
- A connection is two 64 KiB rings, one each way (X servers send replies
  of hundreds of KiB); a write that fills a ring waits for the reader.
  When one side shuts down or closes, the other reads what is left, then
  the end.

## The packets

### netbufs

A packet is a **netbuf** ([`include/net/netbuf.h`](../../kernel/include/net/netbuf.h),
[`net/netbuf.c`](../../kernel/net/netbuf.c)): one 2 KiB buffer, two to a
page, with a small header (`struct netbuf`) at its start and the packet a
window in the rest (`data`, `len`). Going up, each layer takes its
header off the front (`netbuf_pull()`); going down, each puts its header
in the room before the data (`netbuf_push()`, 128 bytes are kept free
for them). Nothing is copied between layers. Freed netbufs go on a free
list and are used again.

The textbook alternative is BSD's mbuf chains, or Linux's `sk_buff`:
packets made of several buffers, so a large one doesn't need a large
buffer. AquilaOS's packets are never larger than an Ethernet frame, so
one buffer each is enough.

### Interfaces

A **network interface** ([`include/net/netif.h`](../../kernel/include/net/netif.h),
[`net/netif.c`](../../kernel/net/netif.c)) is something that sends and
receives Ethernet frames, with its IPv4 configuration: an address, a
netmask, a gateway, flags (`NETIF_UP`: configured; `NETIF_RUNNING`: the
link is up), an MTU (1500) and counters. A driver fills in a
`struct netif` with its MAC address and `transmit` function and
registers it (`netif_register()`); received frames it gives to
`netif_input()`.

- **`lo`**, the loopback interface, is always there and first:
  `127.0.0.1/8`. A packet to one of our own addresses never reaches a
  driver: `ipv4_output()` gives it straight back to the stack
  (`netif_loopback()`).
- **`eth0`**, `eth1`, ...: the cards, in the order they register.

Interfaces are configured from user space with `ioctl()`s on any
`AF_INET` socket ([`net/inet/ifioctl.c`](../../kernel/net/inet/ifioctl.c)):
`SIOCGIFCONF` lists them, `SIOCGIFFLAGS`/`SIOCSIFFLAGS`,
`SIOCGIFADDR`/`SIOCSIFADDR`, `SIOCGIFNETMASK`/`SIOCSIFNETMASK`,
`SIOCGIFBRDADDR`, `SIOCGIFHWADDR`, `SIOCGIFMTU` and `SIOCGIFINDEX` read
and set them, and `SIOCADDRT`/`SIOCDELRT` the default route. Setting is
root's. `aqbox`'s `ifconfig` and `route` use them (`userland/aqbox/net/`).
Or, for the tests and for booting without a configuration step, the
command line: `ip=10.0.2.15/24:10.0.2.2` configures the first Ethernet
interface at boot (`netif_config()`). DHCP is a program, `sdhcp`, run as
a runit service (`/etc/sv/dhcp`), not the kernel's.

### The network's thread

Received frames aren't processed in the driver's interrupt. They are
queued (`rx_queue()`, at most 256 waiting, the rest dropped) for the
**network's thread**, `[net]`, a kernel thread that takes them up the
stack, 32 at a time before letting the others run, and runs the
network's timers: TCP's retransmissions and delayed ACKs, ARP's retries
(`net_callout_set()`).

Why a thread: the work a packet causes (TCP's state machine, copying
data, sending an ACK) is too much for an interrupt, and the kernel runs
with interrupts off (see [Interrupts](interrupts.md)), so a long
handler would hold up everything else. The network thread is **urgent**:
woken by an interrupt, it runs as soon as the interrupt returns, before
the interrupted user thread goes on. This is the job Linux's softirqs
and the BSDs' software interrupts do.

Sending goes the other way, in the system call that sends: `send()`
builds the segment or datagram and goes down the layers to the driver
directly.

## The layers

### Ethernet and ARP

**Ethernet** ([`net/inet/ether.c`](../../kernel/net/inet/ether.c)): a
frame has the destination and source MAC addresses and a type.
`ether_input()` gives ARP frames to ARP and IPv4 packets to IPv4, and
drops the rest; `ether_output()` puts the header on.

**ARP** ([`net/inet/arp.c`](../../kernel/net/inet/arp.c), RFC 826)
finds the MAC address of an IPv4 address on the link. It:

- answers requests for our own address;
- keeps a table of 32 addresses it learned, from requests and replies
  and from the IPv4 packets we receive; when it's full, the entry
  updated longest ago is replaced;
- asks for an address a packet is sent to (a broadcast "who has"), and
  holds up to 3 packets for it until the answer comes, for up to 8
  addresses at a time; the question is asked 3 times, a second apart,
  then the packets are dropped.

An entry that hasn't been heard from for 20 minutes is asked for again:
the machine may have gone, or another may have taken its address.

### IPv4

**IPv4** ([`net/inet/ipv4.c`](../../kernel/net/inet/ipv4.c), RFC 791):

- **Input** checks the header (version, length, checksum), keeps
  packets to one of our addresses, a broadcast or ourselves, and gives
  the payload to ICMP, UDP or TCP by the protocol number. An unknown
  protocol is answered with ICMP "protocol unreachable".
- **Output** puts the header on (TTL 64, "don't fragment") and finds
  the next hop: the destination if it's on the interface's link, else
  the interface's gateway. Then ARP gives the next hop's MAC address,
  and the frame goes.
- **Routing** (`ipv4_route()`): the interface whose link the destination
  is on, else the first one with a gateway. That is the whole routing
  table: a link per interface, and a default route. There is no
  forwarding: AquilaOS is a host, not a router.
- **Fragments** are neither sent nor reassembled: received fragments are
  dropped. TCP never needs them (its segments fit the MTU), and UDP
  datagrams larger than the MTU can't be sent.

### ICMP

**ICMP** ([`net/inet/icmp.c`](../../kernel/net/inet/icmp.c), RFC 792):
echo requests (ping) are answered; echo replies go to the ping socket
whose identifier they carry; "destination unreachable" is sent for a
UDP datagram to a port no socket has, and when one comes back for a
datagram we sent, the sending socket's next call fails with
`ECONNREFUSED` or `EHOSTUNREACH`.

### UDP

**UDP** ([`net/inet/udp.c`](../../kernel/net/inet/udp.c), RFC 768) adds
ports and a checksum to IPv4. A datagram goes to the socket bound to its
destination port (and, if it's connected, from the source it's connected
to; `inet_sock_find()`), and waits in the socket's queue, up to 128 of
them (each counted as a whole 2 KiB netbuf, whatever its size), with its
source address for `recvfrom()`
([`net/inet/socket.c`](../../kernel/net/inet/socket.c)). A socket that
sends without binding first gets an *ephemeral* port, the next free one
from 49152 to 65535.

### TCP

**TCP** (RFC 9293) makes a reliable, ordered byte stream out of IPv4's
packets, which may be lost, duplicated or reordered. It numbers every
byte (the *sequence number*), and the receiver says how far it has
received everything (the *acknowledgment*) and how much more it can take
(the *window*). What isn't acknowledged in time is sent again.

A connection is a **control block**, `struct tcp_pcb`
([`include/net/tcp.h`](../../kernel/include/net/tcp.h)), and goes through
RFC 9293's states: `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`,
then `FIN_WAIT_1`, `FIN_WAIT_2`, `CLOSING`, `TIME_WAIT` on the side that
closes first, `CLOSE_WAIT`, `LAST_ACK` on the other. The code is in
three parts:

- [`tcp_socket.c`](../../kernel/net/inet/tcp_socket.c): the connections
  and their sockets. Every control block is on a list, and once both
  ends are known, in a hash map by its addresses and ports, where
  received segments find it. A control block can outlive its socket: a
  closed socket's connection stays until its data and FIN are sent and
  `TIME_WAIT` (2 × 30 s) is over. A listener's new connections wait for
  `accept()` in its queue, up to its backlog.
- [`tcp_input.c`](../../kernel/net/inet/tcp_input.c): RFC 9293's
  "segment arrives": a segment goes to its connection's state machine,
  or to a listener, which makes a connection for a SYN; a segment for
  no connection is answered with a reset.
- [`tcp_output.c`](../../kernel/net/inet/tcp_output.c): segments from
  the send buffer, retransmission, ACKs.

What it does:

- **Buffers**: 64 KiB each way, in rings. The window it advertises is
  the room left in the receive ring, at most 65535: there is no window
  scaling.
- **Sending**: as much as the peer's window and the congestion window
  let, in segments of the MSS (the peer's MSS option, else 536, and no
  more than the MTU allows). A small segment only goes if it's the last
  of the data and nothing is unacknowledged (**Nagle**'s algorithm,
  unless `TCP_NODELAY`), and never only because the peer's window is
  small (silly window avoidance).
- **Retransmission** (RFC 6298): the round trip time is measured (one
  segment at a time, never a retransmitted one: Karn's rule), and the
  retransmission timeout follows it, between 1 and 60 seconds, doubling
  at each retry. A SYN is retried 5 times, data 8 times; then the
  connection fails with `ETIMEDOUT`.
- **Congestion control** (RFC 5681, RFC 6582): slow start and congestion
  avoidance; three duplicate ACKs retransmit the missing segment at once
  and recover with NewReno; a timeout starts again from one segment.
- **Zero window**: when the peer can take nothing, its window is probed
  with one byte, on the retransmission timer (*persist*).
- **Delayed ACKs** (RFC 1122): every second segment is acknowledged at
  once, others after 100 ms.
- **Out of order**: a segment ahead of what's expected waits in a list
  sorted by sequence number (`ooo_insert()`), and a duplicate ACK tells
  the peer what's missing. When the missing data comes, what waited and
  now follows is taken (`ooo_drain()`). Nothing follows the peer's
  **FIN**: when it's reached, whatever still waits is thrown away
  (`ooo_purge()`); it can only be data the peer sent again, cut
  differently.
- **Keepalive** (`SO_KEEPALIVE`): after 2 hours idle, a probe every 75
  seconds, and the connection fails after 9 unanswered.
- **Initial sequence numbers** are a clock plus a keyed hash of the
  connection's addresses and ports, as RFC 6528 recommends, so they
  can't be guessed.

Received segments and the timers run in the network's thread; sending,
in the system calls of the processes that send. Data from and to user
space goes through a buffer on the kernel's stack, 512 bytes at a time:
copying from a process may fault and sleep, and meanwhile the network
thread changes the rings.

The only options are MSS: no window scaling, timestamps or selective
acknowledgments (SACK).

## The e1000 network card

The **e1000** driver ([`dev/net/e1000.c`](../../kernel/dev/net/e1000.c))
drives Intel's 8254x Ethernet controllers, QEMU's default network card
on the PC (and `-device e1000` on the virt machine), and the 82574L
(QEMU's `e1000e`). It's a PCI device, found by its IDs, its registers
memory-mapped through BAR 0 (`arch_mmio_map()`), its interrupt
installed with `irq_install()`, so nothing in it is PC-specific.

The card moves frames to and from memory itself (DMA), through two
**rings of descriptors**, 32 each: a descriptor is the physical address
of a 2 KiB buffer, a length, and a status.

- **Receiving**: every receive descriptor is given to the card with an
  empty buffer. The card fills one with a frame, marks it done, and
  interrupts; the driver (`rx_poll()`) copies each done frame into a
  netbuf for the stack, and gives the descriptor back by moving the
  ring's tail.
- **Sending**: the frame is copied into the next transmit descriptor's
  buffer and the tail is moved; the card sends it and marks it done. A
  full ring (the next descriptor not done) drops the frame (`ENOBUFS`),
  and TCP sends it again later.
- **Interrupts**: one per batch of received frames, or a change in the
  link. On x86 with an APIC, the 82574L interrupts with MSI; otherwise
  (and on aarch64's virt machine, through the GIC) the PCI line.

Frames are copied once each way, between the card's buffers and
netbufs, which is simpler than giving netbufs to the card.

## What isn't there

- IPv6.
- IP forwarding, multicast, and a routing table beyond each interface's
  link and a default route.
- Fragmentation and reassembly.
- Raw sockets (ping has its own socket type).
- `socketpair()`, and `AF_UNIX` datagram sockets: `socket(AF_UNIX,
  SOCK_DGRAM, 0)` fails with `EPROTONOSUPPORT`.
- TCP window scaling, timestamps and SACK.
- DHCP and DNS in the kernel: they are user space's (`sdhcp`, the C
  library's resolver with `/etc/resolv.conf`).

## Where it is

| File                               | Layer  | What                                          |
|------------------------------------|--------|-----------------------------------------------|
| `kernel/include/net/socket.h`      | kernel | `struct socket`, `struct sock_ops`            |
| `kernel/net/socket.c`              | kernel | the socket layer                              |
| `kernel/net/unix/unix.c`           | kernel | `AF_UNIX` sockets                             |
| `kernel/net/netbuf.c`              | kernel | packets                                       |
| `kernel/net/netif.c`               | kernel | interfaces, `lo`, the network's thread, `ip=` |
| `kernel/net/inet/ether.c`          | kernel | Ethernet, checksums                           |
| `kernel/net/inet/arp.c`            | kernel | ARP                                           |
| `kernel/net/inet/ipv4.c`           | kernel | IPv4, routing                                 |
| `kernel/net/inet/icmp.c`           | kernel | ICMP                                          |
| `kernel/net/inet/udp.c`            | kernel | UDP                                           |
| `kernel/net/inet/socket.c`         | kernel | UDP and ping sockets, ephemeral ports         |
| `kernel/net/inet/ifioctl.c`        | kernel | the interfaces' `ioctl()`s, the default route |
| `kernel/net/inet/tcp_socket.c`     | kernel | TCP: connections, sockets, timers             |
| `kernel/net/inet/tcp_input.c`      | kernel | TCP: segments in                              |
| `kernel/net/inet/tcp_output.c`     | kernel | TCP: segments out, retransmission             |
| `kernel/dev/net/e1000.c`           | kernel | the e1000 card                                |
| `userland/aqbox/net/`              | —      | `ifconfig`, `route`, `ping`                   |

## References

- RFC 791 (IPv4), RFC 792 (ICMP), RFC 768 (UDP), RFC 826 (ARP), RFC 1071
  (the Internet checksum).
- RFC 9293 (TCP, which replaced RFC 793), RFC 6298 (the retransmission
  timer), RFC 5681 (congestion control), RFC 6582 (NewReno), RFC 1122
  (delayed ACKs, keepalive), RFC 6528 (initial sequence numbers).
- W. Richard Stevens, *TCP/IP Illustrated, Volume 1* (the protocols) and,
  with Gary Wright, *Volume 2* (the 4.4BSD implementation: sockets,
  mbufs, the TCP state machine).
- *PCI/PCI-X Family of Gigabit Ethernet Controllers Software Developer's
  Manual*, Intel, 317453: the 8254x's registers and descriptor rings.
  *82574 GbE Controller Family Datasheet*, Intel, 317694.
- *POSIX.1-2017*, `sys/socket.h` and the socket functions.
