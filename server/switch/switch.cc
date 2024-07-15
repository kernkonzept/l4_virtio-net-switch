/*
 * Copyright (C) 2016-2018, 2020, 2023-2024 Kernkonzept GmbH.
 * Author(s): Jean Wolter <jean.wolter@kernkonzept.com>
 *            Alexander Warg <warg@os.inf.tu-dresden.de>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#include "debug.h"
#include "switch.h"
#include "filter.h"

Virtio_switch::Virtio_switch(unsigned max_ports)
: _max_ports{max_ports},
  _max_used{0}
{
  _ports = new Virtio_port *[max_ports]();
}

int
Virtio_switch::lookup_free_slot()
{
  for (unsigned idx = 0; idx < _max_ports; ++idx)
    if (!_ports[idx])
      return idx;

  return -1;
}

bool
Virtio_switch::add_port(Virtio_port *port)
{
  if (!port->mac().is_unknown())
    for (unsigned idx = 0; idx < _max_ports; ++idx)
      if (_ports[idx] && _ports[idx]->mac() == port->mac())
        {
          Dbg(Dbg::Port, Dbg::Warn)
            .printf("Rejecting port '%s'. MAC address already in use.\n",
                    port->get_name());
          return false;
        }

  int idx = lookup_free_slot();
  if (idx < 0)
    return false;

  unsigned uidx = static_cast<unsigned>(idx);
  _ports[uidx] = port;
  if (_max_used == uidx)
    ++_max_used;

  return true;
}

bool
Virtio_switch::add_monitor_port(Virtio_port *port)
{
  if (!_monitor)
    {
      _monitor = port;
      return true;
    }

  Dbg(Dbg::Port, Dbg::Warn).printf("'%s' already defined as monitor port,"
                                   " rejecting monitor port '%s'\n",
                                   _monitor->get_name(), port->get_name());
  return false;
}

void
Virtio_switch::check_ports()
{
  for (unsigned idx = 0; idx < _max_used; ++idx)
    {
      Virtio_port *port = _ports[idx];
      if (port && port->obj_cap() && !port->obj_cap().validate().label())
        {
          Dbg(Dbg::Port, Dbg::Info)
            .printf("Client on port %p has gone. Deleting...\n", port);

          _ports[idx] = nullptr;
          if (idx == _max_used-1)
            --_max_used;

          _mac_table.flush(port);
          delete(port);
        }
    }

  if (   _monitor && _monitor->obj_cap()
      && !_monitor->obj_cap().validate().label())
    {
      delete(_monitor);
      _monitor = nullptr;
    }
}

void
Virtio_switch::drop_pending_at_dest(Virtio_port *src_port)
{
  for (unsigned idx = 0; idx < _max_used; ++idx)
    _ports[idx]->drop_pending(static_cast<Virtio_net *>(src_port));
}

void
Virtio_switch::handle_tx_queue(Virtio_port *port)
{
  auto request = port->get_tx_request();
  if (!request)
    return;

  Mac_addr src = request->src_mac();
  _mac_table.learn(src, port);

  auto dst = request->dst_mac();
  bool is_broadcast = dst.is_broadcast();
  uint16_t vlan = request->has_vlan() ? request->vlan_id() : port->get_vlan();
  if (L4_LIKELY(!is_broadcast))
    {
      auto *target = _mac_table.lookup(dst);
      if (target)
        {
          // Do not send packets to the port they came in; they might
          // be sent to us by another switch which does not know how
          // to reach the target.
          if (target != port && target->match_vlan(vlan))
            {
              target->handle_request(port, request);
              if (_monitor && !filter_request(request.get()))
                _monitor->handle_request(port, request);
            }
          return;
        }
    }

  // It is either a broadcast or an unknown destination - send to all
  // known ports except the source port
  for (unsigned idx = 0; idx < _max_used && _ports[idx]; ++idx)
    {
      auto *target = _ports[idx];
      if (target != port && target->match_vlan(vlan))
        target->handle_request(port, request);
    }

  // Send a copy to the monitor port
  if (_monitor && !filter_request(request.get()))
    _monitor->handle_request(port, request);
}

void
Virtio_switch::handle_port_irq(Virtio_port *port)
{
  /* handle IRQ on one port for the time being */
  if (!port->tx_work_pending() && !port->rx_work_pending())
    Dbg(Dbg::Port, Dbg::Info)
      .printf("Port %s: Irq without pending work\n", port->get_name());

  do
    {
      port->tx_q()->disable_notify();
      port->rx_q()->disable_notify();

      // Within the loop, to trigger before enabling notifications again.
      all_kick_disable_remember();

      try
        {
          // throws Bad_descriptor exceptions raised on SRC port
          while (port->tx_work_pending())
            handle_tx_queue(port);
        }
      catch (L4virtio::Svr::Bad_descriptor &e)
        {
            Dbg(Dbg::Port, Dbg::Warn, "REQ")
              .printf("%s: caught bad descriptor exception: %s - %i"
                      " -- Signal device error on device %p.\n",
                      __PRETTY_FUNCTION__, e.message(), e.error, port);
            port->device_error();
            all_kick_emit_enable();
            return;
        }

      while (port->rx_work_pending())
        port->handle_rx_queue();

      all_kick_emit_enable();

      if (L4_UNLIKELY(port->device_needs_reset()))
        // queue issue flagged during RX handling, e.g. Bad_descriptor
        return;

      port->tx_q()->enable_notify();
      port->rx_q()->enable_notify();

      L4virtio::wmb();
      L4virtio::rmb();
    }
  while (port->tx_work_pending() || port->rx_work_pending());

}
