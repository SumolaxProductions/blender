/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef __BKE_VIRTUAL_NODE_TREE_CXX_H__
#define __BKE_VIRTUAL_NODE_TREE_CXX_H__

/** \file
 * \ingroup bke
 */

#include "DNA_node_types.h"

#include "BLI_string_ref.h"
#include "BLI_array_ref.h"
#include "BLI_map.h"
#include "BLI_vector.h"
#include "BLI_listbase_wrapper.h"
#include "BLI_multi_map.h"
#include "BLI_monotonic_allocator.h"
#include "BLI_string_map.h"

#include "RNA_access.h"

namespace BKE {

using BLI::ArrayRef;
using BLI::IntrusiveListBaseWrapper;
using BLI::Map;
using BLI::MonotonicAllocator;
using BLI::MultiMap;
using BLI::MutableArrayRef;
using BLI::StringMap;
using BLI::StringRef;
using BLI::StringRefNull;
using BLI::Vector;

using bNodeList = IntrusiveListBaseWrapper<struct bNode>;
using bLinkList = IntrusiveListBaseWrapper<struct bNodeLink>;
using bSocketList = IntrusiveListBaseWrapper<struct bNodeSocket>;

class VirtualNode;
class VirtualSocket;
class VirtualLink;

class VirtualNodeTree {
 private:
  bool m_frozen = false;
  Vector<VirtualNode *> m_nodes;
  Vector<VirtualLink *> m_links;
  Vector<VirtualSocket *> m_inputs_with_links;
  MultiMap<std::string, VirtualNode *> m_nodes_by_idname;
  uint m_socket_counter = 0;
  MonotonicAllocator<> m_allocator;

 public:
  void add_all_of_tree(bNodeTree *btree);
  VirtualNode *add_bnode(bNodeTree *btree, bNode *bnode);
  void add_link(VirtualSocket *a, VirtualSocket *b);

  void freeze_and_index();

  ArrayRef<VirtualNode *> nodes()
  {
    return m_nodes;
  }

  ArrayRef<VirtualLink *> links()
  {
    return m_links;
  }

  ArrayRef<VirtualSocket *> inputs_with_links()
  {
    BLI_assert(m_frozen);
    return m_inputs_with_links;
  }

  ArrayRef<VirtualNode *> nodes_with_idname(StringRef idname)
  {
    BLI_assert(m_frozen);
    return m_nodes_by_idname.lookup_default(idname);
  }

  bool is_frozen()
  {
    return m_frozen;
  }

  uint socket_count() const
  {
    return m_socket_counter;
  }

  template<typename T>
  void map_socket_idnames(const StringMap<T> &map,
                          const T &default_value,
                          MutableArrayRef<T> r_result) const
  {
    BLI_assert(r_result.size() == this->socket_count());
    for (const VirtualNode *vnode : m_nodes) {
      for (const VirtualSocket *vsocket : vnode->m_inputs) {
        r_result[vsocket->m_id] = map.lookup_default(vsocket->idname(), default_value);
      }
      for (const VirtualSocket *vsocket : vnode->m_outputs) {
        r_result[vsocket->m_id] = map.lookup_default(vsocket->idname(), default_value);
      }
    }
  }

 private:
  void initialize_direct_links();
  void initialize_links();
  void initialize_nodes_by_idname();
};

class VirtualNode {
 private:
  friend VirtualNodeTree;
  friend VirtualSocket;

  VirtualNodeTree *m_backlink;
  bNodeTree *m_btree;
  bNode *m_bnode;
  MutableArrayRef<VirtualSocket *> m_inputs;
  MutableArrayRef<VirtualSocket *> m_outputs;

 public:
  ArrayRef<VirtualSocket *> inputs()
  {
    return m_inputs;
  }

  ArrayRef<VirtualSocket *> outputs()
  {
    return m_outputs;
  }

  VirtualSocket *input(uint index)
  {
    return m_inputs[index];
  }

  VirtualSocket *output(uint index)
  {
    return m_outputs[index];
  }

  VirtualSocket *input(uint index, StringRef expected_name);
  VirtualSocket *output(uint index, StringRef expected_name);

  bNode *bnode()
  {
    return m_bnode;
  }

  bNodeTree *btree()
  {
    return m_btree;
  }

  ID *btree_id()
  {
    return &m_btree->id;
  }

  PointerRNA rna()
  {
    PointerRNA rna;
    RNA_pointer_create(&m_btree->id, &RNA_Node, m_bnode, &rna);
    return rna;
  }

  StringRefNull name()
  {
    return m_bnode->name;
  }

  StringRefNull idname() const
  {
    return m_bnode->idname;
  }
};

class VirtualSocket {
 private:
  friend VirtualNodeTree;

  VirtualNode *m_vnode;
  bNodeTree *m_btree;
  bNodeSocket *m_bsocket;
  uint m_id;

  MutableArrayRef<VirtualSocket *> m_direct_links;
  MutableArrayRef<VirtualSocket *> m_links;

 public:
  bool is_input() const
  {
    return this->m_bsocket->in_out == SOCK_IN;
  }

  bool is_output() const
  {
    return this->m_bsocket->in_out == SOCK_OUT;
  }

  bNodeSocket *bsocket()
  {
    return m_bsocket;
  }

  bNodeTree *btree()
  {
    return m_btree;
  }

  uint id()
  {
    return m_id;
  }

  ID *btree_id()
  {
    return &m_btree->id;
  }

  VirtualNode *vnode()
  {
    return m_vnode;
  }

  ArrayRef<VirtualSocket *> direct_links()
  {
    BLI_assert(m_vnode->m_backlink->is_frozen());
    return m_direct_links;
  }

  ArrayRef<VirtualSocket *> links()
  {
    BLI_assert(m_vnode->m_backlink->is_frozen());
    return m_links;
  }

  PointerRNA rna()
  {
    PointerRNA rna;
    RNA_pointer_create(&m_btree->id, &RNA_NodeSocket, m_bsocket, &rna);
    return rna;
  }

  StringRefNull name() const
  {
    return m_bsocket->name;
  }

  StringRefNull idname() const
  {
    return m_bsocket->idname;
  }

  StringRefNull identifier() const
  {
    return m_bsocket->identifier;
  }
};

class VirtualLink {
 private:
  friend VirtualNodeTree;

  VirtualSocket *m_from;
  VirtualSocket *m_to;
};

inline VirtualSocket *VirtualNode::input(uint index, StringRef expected_name)
{
  VirtualSocket *vsocket = m_inputs[index];
#ifdef DEBUG
  StringRef actual_name = vsocket->name();
  BLI_assert(actual_name == expected_name);
#endif
  UNUSED_VARS_NDEBUG(expected_name);
  return vsocket;
}

inline VirtualSocket *VirtualNode::output(uint index, StringRef expected_name)
{
  VirtualSocket *vsocket = m_outputs[index];
#ifdef DEBUG
  StringRef actual_name = vsocket->name();
  BLI_assert(actual_name == expected_name);
#endif
  UNUSED_VARS_NDEBUG(expected_name);
  return vsocket;
}

}  // namespace BKE

#endif /* __BKE_VIRTUAL_NODE_TREE_CXX_H__ */
