/******************************************************************************
 * Codb: A Simple Distributed Key-Value Store
 * Author: Pawan Kumar
 * License: MIT License
 * Description: NodeConfig — runtime configuration for a codb server process.
 ******************************************************************************/

#pragma once
#include <map>
#include <string>

// ---------------------------------------------------------------------------
// NodeRole
//
// LEADER   — accepts client writes, replicates out to all peers via
//            LeaderReplicator.  KVServiceImpl has a non-null replicator_.
//
// FOLLOWER — rejects client writes (FAILED_PRECONDITION), receives
//            AppendEntries from the leader via FollowerService.
//            KVServiceImpl is constructed with replicator_ == nullptr.
//
// UNKNOWN  — initial state before the role is determined (future: used in
//            Phase 8 Raft for pre-election period).
// ---------------------------------------------------------------------------
enum class NodeRole
{
    LEADER,
    FOLLOWER,
    UNKNOWN
};

// ---------------------------------------------------------------------------
// NodeConfig — populated from command-line flags in main.cpp
// ---------------------------------------------------------------------------
struct NodeConfig
{
    // Unique, human-readable name for this node.
    // Used as leader_id in AppendEntriesRequest and in log output.
    // e.g. "node1"
    std::string node_id;

    // Address this process binds gRPC to, e.g. "0.0.0.0:50051".
    // Derived from --port flag in main.cpp.
    std::string listen_addr;

    // Role of this node in the current cluster topology.
    // Drives whether LeaderReplicator is created and whether writes are
    // accepted by KVServiceImpl.
    NodeRole role = NodeRole::UNKNOWN;

    // Peer nodes this node should communicate with.
    // Key   = peer node_id   (e.g. "node2")
    // Value = peer gRPC addr (e.g. "localhost:50052")
    //
    // On the leader  : used by LeaderReplicator to build one stub per peer.
    // On a follower  : entry for the leader is used by ReplicationLog for
    //                  RequestEntries catch-up (Phase 3 streaming catch-up).
    std::map<std::string, std::string> peer_addresses;

    // Directory for WAL and other persistent state for this node.
    // Each node MUST have its own separate data_dir — never share between nodes.
    // e.g. "./data/node1"
    std::string data_dir = "./data";
};
