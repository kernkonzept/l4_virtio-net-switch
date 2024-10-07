/*
 * Copyright (C) 2016-2017, 2020, 2022-2024 Kernkonzept GmbH.
 * Author(s): Jean Wolter <jean.wolter@kernkonzept.com>
 *            Alexander Warg <warg@os.inf.tu-dresden.de>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include "port.h"
#include "port_l4virtio.h"
#include "mac_table.h"

/**
 * \ingroup virtio_net_switch
 * \{
 */
/**
 * The Virtio switch contains all ports and processes network requests.
 *
 * A Port on its own is not capable to process an incoming network request
 * because it has no knowledge about other ports. The processing of an incoming
 * request therefore gets delegated to the switch.
 *
 * The `Virtio_switch` is constructed at the start of the Virtio Net Switch
 * application. The factory saves a reference to it to pass it to the
 * `Kick_irq` on port creation.
 */
class Virtio_switch
{
private:
  Port_iface **_ports;  /**< Array of ports. */
  Port_iface *_monitor; /**< The monitor port if there is one. */

  unsigned _max_ports;
  unsigned _max_used;
  Mac_table<> _mac_table;

  int lookup_free_slot();

  /**
   * Deliver a request from a specific port.
   *
   * In case the MAC address of the destination port of a request is not yet
   * present in the `_mac_table` or if the request is a broadcast request, the
   * request is passed to all ports in the same VLAN.
   *
   * \param port  Port whose transmission queue should be processed.
   */
  template<typename REQ>
  void handle_tx_request(Port_iface *port, REQ const &request);

  template<typename PORT>
  void handle_tx_requests(PORT *port);


  void all_rx_notify_emit_and_enable()
  {
    for (unsigned idx = 0; idx < _max_ports; ++idx)
      if (_ports[idx])
        _ports[idx]->rx_notify_emit_and_enable();
  }

  void all_rx_notify_disable_and_remember()
  {
    for (unsigned idx = 0; idx < _max_ports; ++idx)
      if (_ports[idx])
        _ports[idx]->rx_notify_disable_and_remember();
  }

public:
  /**
   * Create a switch with n ports.
   *
   * \param max_ports maximal number of provided ports
   */
  explicit Virtio_switch(unsigned max_ports);

  /**
   * Add a port to the switch.
   *
   * \param port  A pointer to an already constructed Port_iface object.
   *
   * \retval true   Port was added successfully.
   * \retval false  Switch was not able to add the port.
   */
  bool add_port(Port_iface *port);

  /**
   * Add a monitor port to the switch.
   *
   * \param port  A pointer to an already constructed Port_iface object.
   *
   * \retval true   Port was added successfully.
   * \retval false  Switch was not able to add the port.
   */
  bool add_monitor_port(Port_iface *port);

  /**
   * Check validity of ports.
   *
   * Check whether all ports are still used and remove any unused
   * (unreferenced) ports. Shall be invoked after an incoming cap
   * deletion irq to remove ports without clients.
   */
  void check_ports();

  /**
   * Handle an incoming irq on a given port.
   *
   * L4virtio_port does not handle irq related stuff by itself. Someone
   * else has to do this and has to handle incoming irqs. This
   * function is supposed to be invoked after an irq related to the
   * port came in.
   *
   * \param port the L4virtio_port an irq was triggered on
   */
  void handle_l4virtio_port_irq(L4virtio_port *port);

  /**
   * Is there still a free port on this switch available?
   *
   * \param monitor  True if we look for a monitor slot.
   *
   * \retval >=0  The next available port index.
   * \retval -1   No port available.
   */
  int port_available(bool monitor)
  {
    if (monitor)
      return !_monitor ? 0 : -1;

    return lookup_free_slot();
  }
};
/**\}*/
