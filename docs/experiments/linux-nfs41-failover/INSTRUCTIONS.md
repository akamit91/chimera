# Experiment brief: Linux NFSv4.1 client failover across Chimera nodes, with no floating IP

This brief is for an engineer working with Claude Code. Read all of it before
running anything. It is a feasibility experiment: the output is evidence and a
report, and no product code.

## 1. The question

In the intended cluster every Chimera node has its own IP address and there is
no floating address. A client mounts one DNS name. When the node it is using
dies, we want the same mount to carry on through another node, with no manual
remount, no application handling, no kernel change and no custom client.

Find out, with the real Linux kernel client and the real server, whether an
unmodified Linux NFSv4.1 client can do that, and what Chimera would have to
provide. Do not report something as a Linux capability unless a run showed it.

## 2. The server under test

Use the branch `poc-inline-restore` of `https://github.com/akamit91/chimera`
(two commits on top of upstream `3ddeeb9e`). It is a proof of concept and is
not for merge.

What the branch gives you:

- Two Chimera nodes can share NFSv4.1 client, session and reply-cache records.
  The records go through Chimera's key-value interface into `poc/repkv`, a
  small in-memory store that copies every put and delete to one peer over TCP.
- A node that receives a request for a session it does not know rebuilds that
  session from the shared records and keeps the original client ID and session
  ID. It does this once it believes the node that owned the session is dead.
- The config key `server.nfs4_drc_replay_only` selects what a rebuilt session
  may do: 0 carries on as a live session (use this), 1 and 2 only answer
  retries of stored replies.

What the branch does not give you. Do not add any of these; record them as
blockers where they stop an experiment:

- `repkv` has exactly one peer, so the shared-state cluster is two nodes.
- Open and lock state is not shared. After a takeover the client is asked to
  reclaim it.
- There is no cluster manager. A surviving node learns that its peer is dead
  from a heartbeat record, which expires after 1.5 lease periods. The harness
  has a stand-in that tells it at once (section 5).
- Whether the two nodes present one server identity, hand out the same client
  ID, accept one session on both nodes at the same time, or answer
  `fs_locations` is for you to establish (section 6, part B).

## 3. Rules

- Do not change Chimera source, the Linux kernel or NFS protocol behaviour to
  make a test pass. Harness scripts, configuration, captures and documents are
  allowed. Record every temporary configuration change and undo it at the end.
- Do not introduce a floating IP. Two runs below move an address on purpose,
  as a labelled stand-in or a reference point. Never present them as failover
  without a floating IP.
- Source reading alone never earns a PASS. A PASS needs a run.
- Use `vers=4.1` on every mount. Do not use 4.0 or 4.2.
- The client must be a separate machine or VM with its own kernel. A container
  shares the host kernel, so it is not a separate client. Do not use a client
  that other people depend on: these runs hang mounts on purpose.
- Keep site-specific host names and addresses out of every file you write
  under `docs/`. Put them in an environment file outside the repository.
- Commit nothing and push nothing unless the engineer asks. No AI attribution
  in commits.
- Many runs wait on timeouts. With the default `timeo=600` a hung request is
  retried only after 60 seconds. You may mount with `timeo=50,retrans=2` to
  shorten runs; say so in the results, and run the main failure test once with
  the defaults.

## 4. Environment

Server host:

1. Build Chimera from the branch in the Chimera dev image: `make build_release`
   (output in `build/Release`).
2. Build the store module in the same image: `poc/repkv/build.sh`. It writes
   `poc/repkv/build/libchimera_vfs_repkv.so`. The script was only ever run on
   arm64.
3. Run both daemons on one host inside one container. The `linux` backend puts
   a mount ID in its file handles, and two containers give the two daemons
   different IDs, which breaks every file handle after a takeover. Node 1
   listens on port 2049 and node 2 on port 2050.

Distinct address per node. Chimera has no setting for the NFS listen address,
so the harness gives each node its own address on the client side: a DNAT rule
in the client's `nat OUTPUT` chain maps `192.0.2.11:2049` to node 1 and
`192.0.2.12:2049` to node 2. The NFS client and the RPC layer see two servers
at two addresses on port 2049. `192.0.2.21` is a second address of node 1.
State this substitution in the report. If you can give each node a real
address and still keep one backing directory with identical file handles, do
that instead and say how.

Client: record `uname -a`, the distribution and the `nfs-utils` version. It
needs `tcpdump`, `trace-cmd` or tracefs, `python3`, `iptables` with the
`conntrack` match, and passwordless `sudo` over ssh from the server host. The
server host needs `tshark` to decode captures (node 2 is on port 2050:
`-d tcp.port==2050,rpc`).

## 5. Harness

`scripts/` holds a working starting point. Settings come from the environment
file named in `LAB_ENV`; `scripts/env.sh` lists the variables.

| Script | Purpose |
|---|---|
| `srv.sh` | Start or kill a node, and `declare-dead <live> <dead>`, the stand-in for the liveness signal |
| `cli.sh` | On the client: address mappings, mount, name resolution, silent drop of one address, packet capture, kernel trace, transport and session state |
| `workload.py` | Holds a locked file open and loops over mkdir, rmdir, a synchronous overwrite and a synchronous append; reports every slow operation and error, then checks the appended log for lost or repeated lines |
| `run_blackhole.sh` | Drops one client-visible address for a while under load |
| `run_nodekill.sh` | Kills node 1 under load in the variants `single`, `trunk`, `dns` and `restart` |

`srv.sh`, `cli.sh`, `workload.py`, `run_blackhole.sh` and the `single` variant
of `run_nodekill.sh` have been run. The `trunk`, `dns` and `restart` variants
have not. Read each script before you rely on it, and change or replace them
as you need.

Two ways to fail a node, and they are different tests:

- Process crash: `srv.sh kill9`. The client gets a TCP reset and later
  connection refused.
- Dead node: `cli.sh blackhole <addr> on` and then `srv.sh kill9`. The client
  gets silence.

## 6. Experiments

Run each one separately, from freshly started daemons and a fresh mount. Keep
a packet capture, a kernel trace and both node logs for each.

### Part A: the Linux client

**E1. Baseline.** Start node 1. Mount `192.0.2.11:/share` with
`vers=4.1,hard`. Create, write, read back, compare checksums after dropping
the page cache. Record EXCHANGE_ID and CREATE_SESSION: client ID, session ID,
server owner major and minor ID, server scope.

**E2. Client ID trunking.** With E1 mounted, mount `192.0.2.12:/share` at a
second mount point. Record what node 2 returns for the same client owner and
whether Linux treats the two addresses as one server (one or two entries in
`/proc/fs/nfsfs/servers`, one or two sessions). Matching server owners are not
enough evidence; show what the kernel did.

**E3. Session trunking.** Two parts.
(a) One node at two addresses: mount `192.0.2.11` and then `192.0.2.21`, both
with `max_connect=2`. Show whether the second address becomes a second
transport of the same session, whether requests use both, and whether
BIND_CONN_TO_SESSION appears. This isolates the Linux mechanism from the
server's limits.
(b) Two nodes: the same with `192.0.2.11` and `192.0.2.12`.

**E4. Discovery through `fs_locations`.** Mount with
`vers=4.1,trunkdiscovery,max_connect=3,hard`. Show whether Linux asks for
`fs_locations`, what Chimera answers, and what the client does with it. If
Chimera does not implement it, record BLOCKED. You may then check the Linux
side against another server that does implement it, as a separate, labelled
result.

**E5. Discovery without `fs_locations`.** Test separately: the mount name
resolves to both addresses; the mount names one address while the other is
merely reachable; the name's addresses change after the mount. In each case
show which addresses the client connects to, at mount time and after node 1
fails.

**E6. Transport loss with no node failure.** With E3(a) established and the
workload running, silently drop one of the two addresses for at least 90
seconds, then restore it (`run_blackhole.sh`). Do it once for the first
address and once for the second. Show what happens to a request that was
outstanding on the dropped transport, to new requests, and to the workload as
a whole.

**E7. Node failure.** This is the main experiment. Kill node 1 under the
workload and tell node 2 at once. Run all of these, each as a process crash
and as a dead node:
(a) `single`: one transport, to node 1 only.
(b) `dns`: mounted by a name that lists both nodes.
(c) `trunk`: two transports in one session. If E3(b) showed that the two nodes
cannot share a session, use the stand-in in `run_nodekill.sh trunk`: both
addresses reach node 1 until the kill, and at the kill the second address
starts answering from node 2. Label it as a stand-in for a cluster in which
node 2 was in the trunk all along. It answers one question only: whether the
client carries on over the surviving transport while the first address stays
dead.
(d) Reference: after the kill, point node 1's own address at node 2
(`cli.sh map 192.0.2.11 2`). This is what a floating address would do. It
gives the recovery time to compare the others against and it is not a result
for this experiment.

For every run report: whether the mount stayed mounted; whether the workload
continued without any outside action and after how long; errors seen by the
application; the recovery exchange on the wire (EXCHANGE_ID, CREATE_SESSION,
BIND_CONN_TO_SESSION, reclaims, or none); and whether the append log is
complete with nothing repeated. A working `stat` is not evidence of failover.
Leave each hung run for at least three times the mount's major timeout before
calling it hung.

**E8. Failure with a request in flight.** Only meaningful for the E7 runs in
which the client recovered. Kill node 1 while a request is outstanding, for
READ, WRITE, OPEN, CLOSE, LOCK, and for an operation with side effects (mkdir,
rename, remove). For each, show whether the client retried on the same session
and slot, what the surviving node answered (cached reply, re-execution, or an
error), and what the application saw. State plainly where exactly-once does
not hold.

**E9. Node restart.** Start node 1 again after E7(a) has hung
(`run_nodekill.sh ... restart`). Show whether the client reconnects, whether
its old session is accepted or replaced, whether state is reclaimed, and
whether the mount is usable.

### Part B: the Chimera server

From source and from the captures above, state for the branch whether it has:
the same server owner major ID, minor ID and server scope on both nodes; the
same client ID for one client on both nodes; client ID trunking; session
trunking across nodes; BIND_CONN_TO_SESSION; `fs_locations`; shared session
state; shared open and lock state; correct sequence and replay handling after
a takeover. Mark each one present, absent or partial, with the evidence.

## 7. Linux source work

Use the source of the exact kernel the client runs (the upstream tag, and note
any distribution patches you could not check). Start from `fs/nfs/client.c`,
`nfs4client.c`, `nfs4state.c`, `nfs4proc.c`, `nfs4session.c`, and
`net/sunrpc/clnt.c`, `xprt.c`, `xprtmultipath.c`. Trace at least:

- `nfs4_discover_server_trunking`, `nfs41_discover_server_trunking`,
  `nfs41_walk_client_list`: what must match for two addresses to be one client.
- `nfs4_add_trunk`, `nfs4_test_session_trunk`, `nfs4_detect_session_trunking`,
  `rpc_clnt_add_xprt`: when a second address is added, and what `max_connect`
  and `trunkdiscovery` control.
- Transport selection and failure: how a request picks a transport, what
  happens to it when that transport cannot reconnect, and under which
  conditions it moves to another one.
- Session and state recovery in the state manager.

For each conclusion give the file, the function, a short excerpt, the path in
plain words, and whether a run confirmed it. A function that exists is not a
function that runs in this scenario; show from a trace or a capture that it
did.

## 8. Report

Write the report in `docs/experiments/linux-nfs41-failover/`:

- `README.md`: the answers to the nine questions below, the result table, and
  how to repeat the runs.
- `experiment-results.md`: one section per experiment with the kernel version,
  mount options, what was done, what was seen, and the evidence (decoded
  packets, trace lines, log lines). Times in UTC.
- `linux-kernel-analysis.md`: section 7.
- `limitations.md`: every way the rig differs from a real cluster, and
  everything not tested.
- `scripts/`: the scripts as actually run.

Keep captures out of the repository; put short decoded excerpts in the
results.

Result table, one row each: single-node mount; client ID trunking; session
trunking, one node; session trunking, two nodes; `fs_locations` discovery; DNS
multi-address discovery; transport loss with a healthy node; node failure,
single transport; node failure, two transports; in-flight request recovery;
session recovery; node restart. Classify each as PASS (shown by a run), FAIL
(shown not to work), BLOCKED (the server lacks something; name it) or
INCONCLUSIVE (say what evidence is missing).

The report must answer, each with a yes or no first and the evidence after:

1. Can an unmodified Linux NFSv4.1 client keep one mount across a Chimera node
   failure with no floating IP?
2. Is client ID trunking sufficient?
3. Is session trunking necessary for Linux to use more than one address?
4. Is `fs_locations` necessary for automatic discovery with the standard Linux
   mechanism?
5. Can DNS records with several addresses replace `fs_locations`?
6. Can Linux recover if the failed node was its only transport?
7. Can Linux recover an outstanding request without losing exactly-once
   behaviour?
8. What is the minimum Chimera must provide for the desired behaviour?
9. What limits remain even then?

## 9. Done

The experiment is done when every row of the table has a classification backed
by a run or a named blocker, the nine questions are answered, and the client
and server are back in their starting state: mounts gone, address mappings,
drop rules, routes and name entries removed, daemons stopped.
