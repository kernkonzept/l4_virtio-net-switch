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
  Virtio_port **_ports;  /**< Array of ports. */
  Virtio_port *_monitor; /**< The monitor port if there is one. */

  unsigned _max_ports;
  unsigned _max_used;
  Mac_table<> _mac_table;

  int lookup_free_slot();

  /**
   * Deliver the requests from the transmission queue of a specific port.
   *
   * In case the MAC address of the destination port of a request is not yet
   * present in the `_mac_table` or if the request is a broadcast request, the
   * request is passed to all ports in the same VLAN.
   *
   * \param port  Port whose transmission queue should be processed.
   */
  void handle_tx_queue(Virtio_port *port);


  void all_kick_emit_enable()
  {
    for (unsigned idx = 0; idx < _max_ports; ++idx)
      if (_ports[idx])
        _ports[idx]->kick_emit_and_enable();
  }

  void all_kick_disable_remember()
  {
    for (unsigned idx = 0; idx < _max_ports; ++idx)
      if (_ports[idx])
        _ports[idx]->kick_disable_and_remember();
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
   * \param port  A pointer to an already constructed Virtio_port object.
   *
   * \retval true   Port was added successfully.
   * \retval false  Switch was not able to add the port.
   */
  bool add_port(Virtio_port *port);

  /**
   * Add a monitor port to the switch.
   *
   * \param port  A pointer to an already constructed Virtio_port object.
   *
   * \retval true   Port was added successfully.
   * \retval false  Switch was not able to add the port.
   */
  bool add_monitor_port(Virtio_port *port);

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
   * Virtio_port does not handle irq related stuff by itself. Someone
   * else has to do this and has to handle incoming irqs. This
   * function is supposed to be invoked after an irq related to the
   * port came in.
   *
   * \param port the Virtio_port an irq was triggered on
   */
  void handle_port_irq(Virtio_port *port);

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

  void drop_pending_at_dest(Virtio_port *src_port);
};
/**\}*/
