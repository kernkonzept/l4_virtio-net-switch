/*
 * Copyright (C) 2016-2017, 2020, 2022 Kernkonzept GmbH.
 * Author(s): Jean Wolter <jean.wolter@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/cxx/hlist>

#include <l4/l4virtio/server/virtio>
#include <l4/cxx/ref_ptr>
#include <l4/util/assert.h>

#include "virtio_net_buffer.h"
#include "virtio_net.h"
#include "mac_addr.h"
#include "debug.h"
#include "vlan.h"


/**
 * \ingroup virtio_net_switch
 * \{
 */

/**
 * Abstraction for a network request
 *
 * A `Virtio_net_request` is constructed by the source port, using the static
 * function `get_request()` as part of `Virtio_port::get_tx_request()`.
 *
 * On destruction, `finish()` will be called, which, will trigger the client
 * IRQ of the source client.
 */
class Virtio_net_request : public cxx::Ref_obj
{
public:
  typedef cxx::Ref_ptr<Virtio_net_request> Request_ptr;

private:
  /* needed for Virtqueue::finish() */
  /** Source Port */
  Virtio_net *_dev;
  /** transmission queue of the source port */
  L4virtio::Svr::Virtqueue *_queue;
  L4virtio::Svr::Virtqueue::Head_desc _head;

  /* the actual request processor, encapsulates the decoding of the request */
  L4virtio::Svr::Request_processor _req_proc;

  /* A request to the virtio net layer consists of one or more buffers
     containing the Virtio_net::Hdr and the actual packet. To make a
     switching decision we need to be able to look at the packet while
     still being able access the Virtio_net::Hdr for the actual copy
     operation. Therefore we keep track of two locations, the header
     location and the start of the packet (which might be in a
     different buffer) */
  Virtio_net::Hdr *_header;
  Buffer _pkt;

  bool _next_buffer(Buffer *buf)
  { return _req_proc.next(_dev->mem_info(), buf); }

  /**
   * Finalize request
   *
   * This function calls `finish()` on the source port's transmission queue,
   * which will result in triggering the source client IRQ.
   */
  void finish()
  {
    Dbg(Dbg::Virtio, Dbg::Trace).printf("%s(%p)\n", __PRETTY_FUNCTION__, this);
    _queue->finish(_head, _dev, 0);
  }

public:

  /**
   * Get the location and size of the current buffer.
   *
   * \param[out] size   Size of the current buffer.
   *
   * \return  Address of the current buffer.
   *
   * This function returns the address and size of the currently
   * active buffer for this request. The buffer might only be a part
   * of the request, which may consist of more than one buffer.
   */
  const uint8_t *buffer(size_t *size)
  {
    *size = _pkt.left;
    return (uint8_t *)_pkt.pos;
  }

  void dump_request(Virtio_net *dev)
  {
    uint8_t *packet = (uint8_t *)_pkt.pos;
    Dbg debug(Dbg::Request, Dbg::Debug, "REQ");
    if (debug.is_active())
      {
        debug.printf("%p: Next packet: %p:%p - %x bytes\n",
                     dev, _header, packet, _pkt.left);
        if (_header->flags.raw || _header->gso_type)
          {
            debug.cprintf("flags:\t%x\n\t"
                          "gso_type:\t%x\n\t"
                          "header len:\t%x\n\t"
                          "gso size:\t%x\n\t"
                          "csum start:\t%x\n\t"
                          "csum offset:\t%x\n"
                          "\tnum buffer:\t%x\n",
                          _header->flags.raw,
                          _header->gso_type, _header->hdr_len,
                          _header->gso_size,
                          _header->csum_start, _header->csum_offset,
                          _header->num_buffers);
          }
      }
    Dbg pkt_debug(Dbg::Packet, Dbg::Debug, "PKT");
    if (pkt_debug.is_active())
      {
        pkt_debug.cprintf("\t");
        src_mac().print(pkt_debug);
        pkt_debug.cprintf(" -> ");
        dst_mac().print(pkt_debug);
        pkt_debug.cprintf("\n");
        if (Dbg::is_active(Dbg::Packet, Dbg::Trace))
          {
            pkt_debug.cprintf("\n\tEthertype: ");
            uint16_t ether_type = (uint16_t)*(packet + 12) << 8
              | (uint16_t)*(packet + 13);
            char const *protocol;
            switch (ether_type)
              {
              case 0x0800: protocol = "IPv4"; break;
              case 0x0806: protocol = "ARP"; break;
              case 0x8100: protocol = "Vlan"; break;
              case 0x86dd: protocol = "IPv6"; break;
              case 0x8863: protocol = "PPPoE Discovery"; break;
              case 0x8864: protocol = "PPPoE Session"; break;
              default: protocol = nullptr;
              }
            if (protocol)
              pkt_debug.cprintf("%s\n", protocol);
            else
              pkt_debug.cprintf("%04x\n", ether_type);
          }
      }
  }

  // delete copy and assignment
  Virtio_net_request(Virtio_net_request const &) = delete;
  Virtio_net_request &operator = (Virtio_net_request const &) = delete;

  Virtio_net_request(Virtio_net *dev, L4virtio::Svr::Virtqueue *queue,
                     L4virtio::Svr::Virtqueue::Request const &req)
  : _dev(dev), _queue(queue)
  {
    _head = _req_proc.start(_dev->mem_info(), req, &_pkt);

    _header = (Virtio_net::Hdr *)_pkt.pos;
    l4_uint32_t skipped = _pkt.skip(sizeof(Virtio_net::Hdr));

    if (L4_UNLIKELY(   (skipped != sizeof(Virtio_net::Hdr))
                    || (_pkt.done() && !_next_buffer(&_pkt))))
      {
        _header = 0;
        Dbg(Dbg::Queue, Dbg::Warn).printf("Invalid request\n");
        return;
      }
  }

  ~Virtio_net_request()
  { finish(); }

  bool valid() const
  { return _header != 0; }

  /**
   * Drop all requests of a specific queue.
   *
   * This function is used for example to drop all requests in the transmission
   * queue of a monitor port, since monitor ports are not allowed to transmit
   * data.
   *
   * \param dev    Port of the provided virtqueue.
   * \param queue  Virtqueue to drop all requests of.
   */
  static void drop_requests(Virtio_net *dev,
                            L4virtio::Svr::Virtqueue *queue)
  {
    if (L4_UNLIKELY(!queue->ready()))
      return;

    if (queue->desc_avail())
      Dbg(Dbg::Request, Dbg::Debug)
        .printf("Dropping incoming packets on monitor port\n");

    L4virtio::Svr::Request_processor req_proc;
    Buffer pkt;

    while (auto req = queue->next_avail())
      {
        auto head = req_proc.start(dev->mem_info(), req, &pkt);
        queue->finish(head, dev, 0);
      }
  }

  /**
   * Construct a request from the next entry of a provided queue.
   *
   * \param dev    Port of the provided virtqueue.
   * \param queue  Virtqueue to extract next entry from.
   */
  static Request_ptr get_request(Virtio_net *dev,
                                 L4virtio::Svr::Virtqueue *queue)
  {
    if (L4_UNLIKELY(!queue->ready()))
      return nullptr;

    if (auto r = queue->next_avail())
      {
        // Virtio_net_request keeps "a lot of internal state",
        // therefore we create the object before creating the
        // state.
        // We might check later on whether it is possible to
        // save the state when we actually have to because a
        // transfer is blocking on a port.
        auto request = cxx::make_ref_obj<Virtio_net_request>(dev, queue, r);
        if (request->valid())
          {
            request->dump_request(dev);
            return request;
          }
      }
    return nullptr;
  }

  Buffer const &first_buffer() const
  { return _pkt; }

  Virtio_net::Hdr const *header() const
  { return _header; }

  /** Get the Mac address of the destination port. */
  Mac_addr dst_mac() const
  {
    return (_pkt.pos && _pkt.left >= Mac_addr::Addr_length)
      ? Mac_addr(_pkt.pos)
      : Mac_addr(Mac_addr::Addr_unknown);
  }

  /** Get the Mac address of the source port. */
  Mac_addr src_mac() const
  {
    return (_pkt.pos && _pkt.left >= Mac_addr::Addr_length * 2)
      ? Mac_addr(_pkt.pos + Mac_addr::Addr_length)
      : Mac_addr(Mac_addr::Addr_unknown);
  }

  bool has_vlan() const
  {
    if (!_pkt.pos || _pkt.left < 14)
      return false;

    uint8_t *p = reinterpret_cast<uint8_t*>(_pkt.pos);
    return p[12] == 0x81U && p[13] == 0x00U;
  }

  uint16_t vlan_id() const
  {
    if (!has_vlan() || _pkt.left < 16)
      return VLAN_ID_NATIVE;

    uint8_t *p = reinterpret_cast<uint8_t*>(_pkt.pos);
    return ((uint16_t)p[14] << 8 | (uint16_t)p[15]) & 0xfffU;
  }

  L4virtio::Svr::Request_processor const &get_request_processor() const
  { return _req_proc; }

  Virtio_net const *dev() const
  { return _dev; }
};
/**\}*/
