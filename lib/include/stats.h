/*
 * Copyright (C) 2020, 2025 Kernkonzept GmbH.
 * Author(s): Jan Klötzke <jan.kloetzke@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/sys/capability>
#include <l4/sys/cxx/ipc_iface>
#include <l4/re/dataspace>
#include <l4/re/util/unique_cap>

#include <cstring>
#include <memory>

/**
 * \ingroup virtio_net_switch
 * \file
 * Client interface for port statistics
 */

namespace Virtio_net_switch {

/// Statistics for one port.
struct Port_statistics
{
  l4_uint64_t tx_num;        ///< number of successful send requests
  l4_uint64_t tx_dropped;    ///< number of dropped send request
  l4_uint64_t tx_bytes;      ///< bytes successfully sent
  l4_uint64_t rx_num;        ///< number of successful receive requests
  l4_uint64_t rx_dropped;    ///< number of dropped receive requests
  l4_uint64_t rx_bytes;      ///< bytes successfully received
  unsigned char mac[6];      ///< MAC address of a port
  char          name[20];    ///< name of a port
  unsigned char in_use;      ///< 1 iff the data structure is currently
                             ///< in use, 0 otherwise
};

/// Base statistics data structure, resides at the beginning of shared memory
struct Statistics
{
  l4_uint64_t age;        ///< This value increases on any change in the data
                          ///< structure. E.g. when a port on the switch is
                          ///< created or discarded.
  l4_uint64_t max_ports;  ///< The maximum number of ports that the switch
                          ///< supports.
  struct Port_statistics port_stats[]; ///< Array of Port_statistics.
};

/**
 * IPC interface for the statistics interface
 *
 * This interface is plumbing used by the Monitor interface. Clients usually
 * do not invoke this directly.
 */
class Statistics_if : public L4::Kobject_t<Statistics_if, L4::Kobject>
{
public:
  /**
   * Get shared memory buffer containing port statistics.
   *
   * \param[out] ds Capability of the dataspace containing port statistics.
   *
   * \retval 0    Success
   * \retval <0   Error
   */
  L4_INLINE_RPC(long, get_buffer, (L4::Ipc::Out<L4::Cap<L4Re::Dataspace> > ds));

  /**
   * Instruct the switch to update the statistics information in the shared
   * memory buffer.
   *
   * \retval 0    Success
   * \retval <0   Error
   */
  L4_INLINE_RPC(long, sync, ());

  typedef L4::Typeid::Rpcs<get_buffer_t, sync_t> Rpcs;
};

/**
 * Statistics client interface.
 *
 * A client can retrieve per port statistics information, such as the number
 * of sent and received packets. The information is shared with a
 * L4Re::Dataspace. The information contained in the L4Re::Dataspace is
 * updated by the switch only on Monitor::sync().
 */
class Monitor
{
public:
  Monitor(L4Re::Util::Unique_del_cap<Statistics_if> cap)
  : _cap(std::move(cap))
  {
    _ds = L4Re::Util::make_unique_cap<L4Re::Dataspace>();
    L4Re::chksys(_cap->get_buffer(_ds.get()),
                 "Could not get stats dataspace from switch.");
    void *addr;
    L4Re::chksys(L4Re::Env::env()->rm()->attach(&addr, _ds->size(),
                                                L4Re::Rm::F::Search_addr | L4Re::Rm::F::R,
                                                L4::Ipc::make_cap(_ds.get(), L4_CAP_FPAGE_RO)),
                 "Could not attach stats dataspace.");
    _stats = reinterpret_cast<Virtio_net_switch::Statistics *>(addr);
  }

  ~Monitor()
  {
    L4Re::Env::env()->rm()->detach(reinterpret_cast<l4_addr_t>(_stats), 0);
  }

  /**
   * Retrieve pointer to Port_statistics structure
   *
   * \param name  The name of the port.
   * \return      Pointer to the Port_statistics structure or `nullptr` if no
   *              port of that name was found.
   */
  Port_statistics *get_port_stats(char const *name) const
  {
    for(size_t i = 0; i < _stats->max_ports; ++i)
      {
        if (_stats->port_stats[i].in_use
            && !strncmp (_stats->port_stats[i].name, name,
                         sizeof(_stats->port_stats[i].name)))
          return &_stats->port_stats[i];
      }
    return nullptr;
  }

  /**
   * Retrieve the MAC address of a port
   *
   * \param      name   The name of the port.
   * \param[out] mac    A 6 byte buffer allocated by the caller. To be filled
   *                    with the MAC address of the port.
   */
  bool get_port_mac(char const *name, unsigned char *mac) const
  {
    for (l4_uint64_t i = 0; i < _stats->max_ports; ++i)
      {
        if (_stats->port_stats[i].in_use
            && !strncmp (_stats->port_stats[i].name, name, 20))
          {
            std::memcpy(mac, _stats->port_stats[i].mac, 6);
            return true;
          }
      }
    return false;
  }

  /**
   * Instruct the network switch to update the statistics information.
   */
  void sync() const
  {
    L4Re::chksys(_cap->sync(),
                 "Synchronizing statistics information failed.\n");
  }

  l4_uint64_t age() const
  { return _stats->age; }

private:
  L4Re::Util::Unique_cap<L4Re::Dataspace> _ds;
  L4Re::Util::Unique_del_cap<Statistics_if> _cap;
  Statistics *_stats;
};

class Port_monitor
{
private:
  std::shared_ptr<Monitor> _m;
  char _name[20];
  l4_uint64_t _age;
  Port_statistics *_stats; //only valid for the given age

public:
  /**
   * The statistics information is only valid after calling Monitor::sync().
   */
  Port_monitor(std::shared_ptr<Monitor> m, char const *name)
  : _m(m)
  {
    strncpy(_name, name, sizeof(_name) - 1);
    _name[sizeof(_name) - 1] = '\0';
    _age = _m->age();
    _stats = _m->get_port_stats(_name);
  }

  /**
   * Get statistics for this port.
   *
   * *NOTE* This data is updated on Monitor::sync(). It is up to the client to
   *  call sync() appropriately.
   */
  void stats(l4_uint64_t *tx_num,
             l4_uint64_t *tx_dropped,
             l4_uint64_t *tx_bytes,
             l4_uint64_t *rx_num,
             l4_uint64_t *rx_dropped,
             l4_uint64_t *rx_bytes)
  {
    // ports have changed
    if (_age != _m->age())
      {
        _stats = _m->get_port_stats(_name);
        _age = _m->age();
      }

    // no port found
    if (!_stats)
      {
        tx_num = tx_dropped = tx_bytes = rx_num = rx_dropped = rx_bytes = 0;
        return;
      }

    *tx_num = _stats->tx_num;
    *tx_dropped = _stats->tx_dropped;
    *tx_bytes = _stats->tx_bytes;
    *rx_num = _stats->rx_num;
    *rx_dropped = _stats->rx_dropped;
    *rx_bytes = _stats->rx_bytes;
  }
};
}
