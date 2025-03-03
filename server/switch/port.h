/*
 * Copyright (C) 2016-2018, 2020, 2022-2024 Kernkonzept GmbH.
 * Author(s): Jean Wolter <jean.wolter@kernkonzept.com>
 *            Alexander Warg <warg@os.inf.tu-dresden.de>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include "virtio_net.h"
#include "request.h"
#include "transfer.h"
#include "mac_addr.h"
#include "vlan.h"

#include <set>
#include <vector>

/**
 * \ingroup virtio_net_switch
 * \{
 */
/**
 * A Port on the Virtio Net Switch
 *
 * A Port object gets created by `Virtio_factory::op_create()`. This function
 * actually only instantiates objects of the types `Switch_port` and
 * `Monitor_port`. The created Port registers itself at the switch's server.
 * Usually, the IPC call for port creation comes from ned. To finalize the
 * setup, the client has to initialize the port during the virtio
 * initialization phase. To do this, the client registers a dataspace for
 * queues and buffers and provides an IRQ to notify the client on incoming
 * network requests.
 */
class Virtio_port : public Virtio_net
{
  /*
   * VLAN related management information.
   *
   * A port may either be
   *  - a native port (_vlan_id == VLAN_ID_NATIVE), or
   *  - an access port (_vlan_id set accordingly), or
   *  - a trunk port (_vlan_id == VLAN_ID_TRUNK, _vlan_bloom_filter and
   *    _vlan_ids populated accordingly).
   */
  l4_uint16_t _vlan_id = VLAN_ID_NATIVE; // VID for native/access port
  l4_uint32_t _vlan_bloom_filter = 0; // Bloom filter for trunk ports
  std::set<l4_uint16_t> _vlan_ids;  // Authoritative list of trunk VLANs

  inline l4_uint32_t vlan_bloom_hash(l4_uint16_t vid)
  { return 1UL << (vid & 31U); }

  Mac_addr _mac;  /**< The MAC address of the port. */
  char _name[20]; /**< Debug name */

public:
  // delete copy and assignment
  Virtio_port(Virtio_port const &) = delete;
  Virtio_port &operator = (Virtio_port const &) = delete;

  char const *get_name() const
  { return _name; }

  l4_uint16_t get_vlan() const
  { return _vlan_id; }

  inline bool is_trunk() const
  { return _vlan_id == VLAN_ID_TRUNK; }

  inline bool is_native() const
  { return _vlan_id == VLAN_ID_NATIVE; }

  inline bool is_access() const
  { return !is_trunk() && !is_native(); }

  /**
   * Set port as access port for a certain VLAN.
   *
   * \param id  The VLAN id for traffic on this port (0 < id < 0xfff)
   *
   * The port does not see VLAN tags but belongs to the given VLAN.
   */
  void set_vlan_access(l4_uint16_t id)
  {
    assert(vlan_valid_id(id));
    _vlan_id = id;
    _vlan_bloom_filter = 0;
    _vlan_ids.clear();
  }

  /**
   * Set port as trunk port.
   *
   * \param ids List of VLAN ids that are switched on this port
   *
   * Incoming traffic on this port is expected to have a VLAN tag that matches
   * one in \a ids. Outgoing traffic will be tagged it if there is no tag in
   * the Ethernet header yet.
   */
  void set_vlan_trunk(const std::vector<l4_uint16_t> &ids)
  {
    // bloom filter to quickly reject packets that do not belong to this port
    l4_uint32_t filter = 0;

    _vlan_ids.clear();
    for (const auto id : ids)
      {
        assert(vlan_valid_id(id));
        filter |= vlan_bloom_hash(id);
        _vlan_ids.insert(id);
      }

    _vlan_id = VLAN_ID_TRUNK;
    _vlan_bloom_filter = filter;
  }

  /**
   * Set this port as monitor port.
   *
   * Ensures that outgoing traffic will have a VLAN tag if the packet belongs
   * to a VLAN. Packets coming from native ports will remain untagged.
   */
  void set_monitor()
  {
    _vlan_id = VLAN_ID_TRUNK;
    _vlan_bloom_filter = 0;
  }

  /**
   * Match VLAN id.
   *
   * \param id  The VLAN id of the packet or VLAN_ID_NATIVE.
   *
   * Check whether VLAN \a id is switched on this port. Packets of native ports
   * have the special VLAN_ID_NATIVE id.
   */
  bool match_vlan(uint16_t id)
  {
    // Regular case native/access port
    if (id == _vlan_id)
      return true;

    // Quick check: does port probably accept this VLAN?
    if ((_vlan_bloom_filter & vlan_bloom_hash(id)) == 0)
      return false;

    return _vlan_ids.find(id) != _vlan_ids.end();
  }

  /**
   * Get MAC address.
   *
   * Might be Mac_addr::Addr_unknown if this port has no explicit MAC address
   * set.
   */
  inline Mac_addr mac() const
  { return _mac; }

  /**
   * Create a Virtio net port object
   */
  explicit Virtio_port(unsigned vq_max, unsigned num_ds, char const *name,
                       l4_uint8_t const *mac)
  : Virtio_net(vq_max),
    _mac(Mac_addr::Addr_unknown)
  {
    init_mem_info(num_ds);

    strncpy(_name, name, sizeof(_name));
    _name[sizeof(_name) - 1] = '\0';

    Features hf = _dev_config.host_features(0);
    if (mac)
      {
        _mac = Mac_addr((char const *)mac);
        memcpy((void *)_dev_config.priv_config()->mac, mac,
               sizeof(_dev_config.priv_config()->mac));

        hf.mac() = true;
        Dbg d(Dbg::Port, Dbg::Info);
        d.cprintf("%s: Adding Mac '", _name);
        _mac.print(d);
        d.cprintf("' to host features to %x\n", hf.raw);
      }
    _dev_config.host_features(0) = hf.raw;
    _dev_config.reset_hdr();
    Dbg(Dbg::Port, Dbg::Info)
      .printf("%s: Set host features to %x\n", _name,
              _dev_config.host_features(0));
  }

  /** Check whether there is any work pending on the transmission queue */
  bool tx_work_pending() const
  {
    return L4_LIKELY(tx_q()->ready()) && tx_q()->desc_avail();
  }

  /** Get one request from the transmission queue */
  std::optional<Virtio_net_request> get_tx_request()
  {
    auto ret = Virtio_net_request::get_request(this, tx_q());

    /*
     * Trunk ports are required to have a VLAN tag and only accept packets that
     * belong to a configured VLAN. Access ports must not be VLAN tagged to
     * prevent double tagging attacks. Otherwise the packet is dropped.
     */
    if (ret)
      {
        if (is_trunk())
          {
            if (_vlan_ids.find(ret->vlan_id()) == _vlan_ids.end())
              return std::nullopt;
          }
        else if (is_access() && ret->has_vlan())
          return std::nullopt;
      }

    return ret;
  }

  /**
   * Drop all requests pending in the transmission queue.
   *
   * This is used for monitor ports, which are not allowed to send packets.
   */
  void drop_requests()
  { Virtio_net_request::drop_requests(this, tx_q()); }

  /**
   * Handle a request  - send it to the guest associated with this port
   *
   * We try to send a packet to the guest associated with this
   * port. To do that, we create a Virtio_net_transfer object to keep
   * any state related to this transaction. If the transfer is
   * successful, we delete the transfer object.
   */
  void handle_request(Virtio_port *src_port,
                      Virtio_net_request const &request)
  {
    Virtio_vlan_mangle mangle;

    if (is_trunk())
      {
        /*
         * Add a VLAN tag only if the packet does not already have one (by
         * coming from another trunk port) or if the packet does not belong to
         * any VLAN (by coming from a native port). The latter case is only
         * relevant if this is a monitor port. Otherwise traffic from native
         * ports is never forwarded to trunk ports.
         */
        if (!src_port->is_trunk() && !src_port->is_native())
          mangle = Virtio_vlan_mangle::add(src_port->_vlan_id);
      }
    else
      /*
       * Remove VLAN tag only if the packet actually has one (by coming from a
       * trunk port).
       */
      if (src_port->is_trunk())
        mangle = Virtio_vlan_mangle::remove();

    // throws Bad_descriptor exception raised in SRC port queue.
    switch (Virtio_net_transfer::transfer(request, this, rx_q(), mangle))
      {
      case Virtio_net_transfer::Result::Delivered: [[fallthrough]];
      case Virtio_net_transfer::Result::Exception: return;
      case Virtio_net_transfer::Result::Dropped: break;
      }

    // Drop packet that could not be transferred
  }
};
/**\}*/
