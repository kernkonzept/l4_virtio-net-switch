/*
 * Copyright (C) 2016-2017, 2020, 2022-2024 Kernkonzept GmbH.
 * Author(s): Jean Wolter <jean.wolter@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <array>
#include <map>
#include <algorithm>
#include "mac_addr.h"
/**
 * \ingroup virtio_net_switch
 * \{
 */

/**
 * Mac_table manages a 1:n association between ports and MAC addresses.
 *
 * There are different types of devices which might be attached to a
 * port. For a normal device the switch sees exactly one MAC address
 * per port - the MAC address of the device attached to it. But there
 * might be other devices like software bridges attached to the port
 * sending packets with different MAC addresses to the port. Therefore
 * the switch has to manage a 1:n association between ports and MAC
 * addresses. The MAC table manages this association.
 *
 * When a packet comes in we need to find the destination port for the
 * packet and therefore perform a lookup based on the MAC address.
 *
 * To prevent unbounded grow of the lookup table the number of entries is
 * limited. Replacement is done on a round-robin basis. If the capacity was
 * reached the oldest entry is evicted.
 */
template<std::size_t Size = 1024U>
class Mac_table
{
public:
  Mac_table()
  : _mac_table(),
    _entries(),
    _rr_index(0U)
  {}

  /**
   * Find the destination port for a MAC address.
   *
   * \param dst  MAC address
   *
   * \retval nullptr  The MAC address is not known (yet)
   * \retval other    Pointer to the destination port
   */
  Virtio_port *lookup(Mac_addr dst) const
  {
    auto entry = _mac_table.find(dst);
    return (entry != _mac_table.end()) ? entry->second->port : nullptr;
  }

  /**
   * Learn a MAC address (add it to the MAC table).
   *
   * \param src   MAC address
   * \param port  Pointer to the port object that can be used to reach
   *              MAC address src
   *
   * Will evict the oldest learned address from the table if the maximum
   * capacity was reached and if the MAC address was not known yet. The source
   * port of the table entry is always updated to cope with clients that move
   * between ports.
   */
  void learn(Mac_addr src, Virtio_port *port)
  {
    Dbg info(Dbg::Port, Dbg::Info);

    if (L4_UNLIKELY(info.is_active()))
      {
        // check whether we already know about src mac
        auto *p = lookup(src);
        if (!p || p != port)
          {
            info.printf("%s %-20s -> ", !p ? "learned " : "replaced",
                        port->get_name());
            src.print(info);
            info.cprintf("\n");
          }
      }

    auto status = _mac_table.emplace(src, &_entries[_rr_index]);
    if (L4_UNLIKELY(status.second))
      {
        if (_entries[_rr_index].port)
          {
            // remove old entry
            _mac_table.erase(_entries[_rr_index].addr);
          }
        // Set/Replace port and mac address
        _entries[_rr_index].port = port;
        _entries[_rr_index].addr = src;
        _rr_index = (_rr_index + 1U) % Size;
      }
    else
      {
        // Update port to allow for movement of client between ports
        status.first->second->port = port;
      }
  }

  /**
   * Flush all associations with a given port.
   *
   * \param port  Pointer to port that is to be flushed
   *
   * This function removes all references to a given port from the MAC
   * table. Since we manage a 1:n association between ports and MAC
   * addresses there might be more than one entry for a given port and
   * we have to iterate over the whole array to delete every reference
   * to the port.
   */
  void flush(Virtio_port *port)
  {
    typedef std::pair<const Mac_addr, Entry*> TableEntry;

    auto iter = _mac_table.begin();
    while ((iter = std::find_if(iter, _mac_table.end(),
                                [port](TableEntry const &p)
                                { return p.second->port == port; }))
           != _mac_table.end())
      {
        iter->second->port = nullptr;
        iter->second->addr = Mac_addr::Addr_unknown;
        iter = _mac_table.erase(iter);
      }

    assert(std::find_if(_mac_table.begin(), _mac_table.end(),
                        [port](TableEntry const &p)
                        { return p.second->port == port; }) == _mac_table.end());
  }

private:
  /**
   * Value class for MAC table entry.
   *
   * The instances hold the actual key (addr) to know which _mac_table entry
   * points there.
   */
  struct Entry {
    Virtio_port *port;
    Mac_addr addr;

    Entry()
    : port(nullptr),
      addr(Mac_addr::Addr_unknown)
    {}
  };

  std::map<Mac_addr, Entry*> _mac_table;
  std::array<Entry, Size> _entries;
  size_t _rr_index;
};
/**\}*/
