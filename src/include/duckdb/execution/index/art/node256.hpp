//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/node256.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

//! Node256 holds up to 256 Node children which can be indexed by the key byte.
class Node256 {
public:
	Node256() = delete;
	Node256(const Node256 &) = delete;
	Node256 &operator=(const Node256 &) = delete;

	//! Number of non-null children
	uint16_t count;
	//! Node pointers to the child nodes
	Node children[Node::NODE_256_CAPACITY];

public:
	//! Get a new Node256, might cause a new buffer allocation, and initialize it
	static Node256 &New(ART &art, Node &node);
	//! Free the node (and its subtree)
	static void Free(ART &art, Node &node);

	//! Initializes all the fields of the node while growing a Node48 to a Node256
	static Node256 &GrowNode48(ART &art, Node &node256, Node &node48);

	//! Initializes a merge by incrementing the buffer IDs of the node
	void InitializeMerge(ART &art, const unsafe_vector<idx_t> &upper_bounds);

	//! Insert a child node at byte
	static void InsertChild(ART &art, Node &node, const uint8_t byte, const Node child);
	//! Delete the child node at byte
	static void DeleteChild(ART &art, Node &node, const uint8_t byte);

	//! Replace the child node at byte
	inline void ReplaceChild(const uint8_t byte, const Node child) {
		auto was_gate = children[byte].IsGate();
		children[byte] = child;
		if (was_gate && child.HasMetadata()) {
			children[byte].SetGate();
		}
	}

	//! Get the (immutable) child for the respective byte in the node
	const Node *GetChild(const uint8_t byte) const;
	//! Get the child for the respective byte in the node
	Node *GetChildMutable(const uint8_t byte);
	//! Get the first (immutable) child that is greater or equal to the specific byte
	const Node *GetNextChild(uint8_t &byte) const;
	//! Get the first child that is greater or equal to the specific byte
	Node *GetNextChildMutable(uint8_t &byte);

	//! Vacuum the children of the node
	void Vacuum(ART &art, const unordered_set<uint8_t> &indexes);

	//! Transform the children of the node.
	void TransformToDeprecated(ART &art, unsafe_unique_ptr<FixedSizeAllocator> &allocator);
};
} // namespace duckdb
