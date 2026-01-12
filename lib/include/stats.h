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

namespace Virtio_net_switch {

// Statistics for one port.
struct Port_statistics
{
  l4_uint64_t tx_num;        // number of successful send requests
  l4_uint64_t tx_dropped;    // number of dropped send request
  l4_uint64_t tx_bytes;      // bytes successfully sent
  l4_uint64_t rx_num;        // number of successful receive requests
  l4_uint64_t rx_dropped;    // number of dropped receive requests
  l4_uint64_t rx_bytes;      // bytes successfully received
  unsigned char mac[6];      // MAC address of a port
  char          name[20];    // name of a port
  unsigned char in_use;      // 1 iff the data structure is currently
                             // in use, 0 otherwise
};

// Base statistics data structure, resides at the beginning of shared memory
struct Statistics
{
  // This value increases on any change in the data structure. E.g. when a
  // port on the switch is created or discarded.
  l4_uint64_t age;
  // The maximum number of ports that the switch supports.
  l4_uint64_t max_ports;
  struct Port_statistics port_stats[];
};

class Statistics_if : public L4::Kobject_t<Statistics_if, L4::Kobject, 1>
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
 * Client interface.
 *
 * The data is only updated on Monitor::sync().
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
