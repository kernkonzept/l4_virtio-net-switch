/*
 * Copyright (C) 2016-2017, 2020, 2022-2024 Kernkonzept GmbH.
 * Author(s): Jean Wolter <jean.wolter@kernkonzept.com>
 *            Alexander Warg <warg@os.inf.tu-dresden.de>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include "virtio_net.h"
#include "request.h"
#include "vlan.h"

#include <vector>

#include <l4/cxx/ipc_timeout_queue>

#include <l4/cxx/ref_ptr>
#include <l4/cxx/dlist>
#include <l4/cxx/pair>

/**
 * \ingroup virtio_net_switch
 * \{
 */

/**
 * A network request to only a single destination.
 *
 * A `Virtio_net_request` can have multiple destinations (being a broadcast
 * request, for example). That is why it is processed by multiple
 * `Virtio_net_transfer`s, each representing the delivery to a single
 * destination port.
 *
 * `Virtio_port::handle_request` constructs one `Virtio_net_transfer` for each
 * destination of the request.
 *
 * On destruction, `finish_transfer()` will be called, which, in case of a
 * successful delivery, will trigger the client IRQ of the destination client.
 */
class Virtio_net_transfer :
  public cxx::D_list_item,
  public L4::Ipc_svr::Timeout_queue::Timeout
{
public:
  typedef cxx::D_list<Virtio_net_transfer> Pending_list;

private:
  /*
   * src description
   *
   * We already looked at the very first buffer to find the target of
   * the packet. The request processor of the "parent request"
   * contains the current state of the transaction up to this
   * point. Since there might be more then one target for the request
   * we have to keep track of our own state and need our own request
   * processor instance, which will be initialized using the current
   * state of the "parent request".
   */
  /** The associated network request */
  Virtio_net_request::Request_ptr _request;
  L4virtio::Svr::Request_processor _src_req_proc;

  /* dst description */
  /** The destination port */
  Virtio_net *_dst_dev;
  /** The Receive queue of the destination port */
  L4virtio::Svr::Virtqueue *_dst_queue;
  L4virtio::Svr::Virtqueue::Head_desc _dst_head;
  L4virtio::Svr::Request_processor _dst_req_proc;
  Virtio_net::Hdr *_dst_header = nullptr;

  /* the buffer descriptors used for this transaction and the amount
     of bytes copied to the current target descriptor */
  Buffer _src;
  Buffer _dst;
  unsigned _total;

  /* Data structures used to merge rx buffers */
  /* the list of target descriptors merged into one request for the target */
  typedef cxx::Pair<L4virtio::Svr::Virtqueue::Head_desc, l4_uint32_t> Consumed_entry;
  std::vector<Consumed_entry> _consumed;
  l4_uint16_t _num_merged = 0;

  Virtio_vlan_mangle _mangle;

  bool next_src_buffer()
  { return _src_req_proc.next(_request->dev()->mem_info(), &_src); }

  bool next_dst_buffer()
  { return _dst_req_proc.next(_dst_dev->mem_info(), &_dst); }

  /**
   * Callback for the timeout.
   *
   * If `transfer()` does not return successfully (that is, when the
   * destination queue is full), `Virtio_port::handle_request()` enqueues this
   * transfer in its list of pending requests and additionally starts a
   * timeout. On expiration, this function removes the transfer from the list
   * of pending requests. Finally, the transfer gets deleted.
   */
  void expired()
  {
    Dbg(Dbg::Queue, Dbg::Debug, "Queue").printf("Timeout expired: %p\n", this);

    Pending_list::remove(this);
    delete this;
  }

public:
  // delete copy and assignment
  Virtio_net_transfer(Virtio_net_transfer const &) = delete;
  Virtio_net_transfer &operator = (Virtio_net_transfer const &) = delete;

  Virtio_net_transfer(Virtio_net_request::Request_ptr request,
                      Virtio_net *dst_dev, L4virtio::Svr::Virtqueue *dst_queue,
                      const Virtio_vlan_mangle &mangle)
  : _request{request},
    _src_req_proc{request->get_request_processor()},
    _dst_dev{dst_dev},
    _dst_queue{dst_queue},
    _src{_request->first_buffer()},
    _mangle{mangle}
  {}

  /**
   * Deliver the request to the destination port
   *
   * \retval true   The request has been delivered to the destination port.
   * \retval false  The request could not be delivered to the destination port.
   */
  bool transfer()
  {
    Dbg trace(Dbg::Request, Dbg::Trace, "REQ");
    trace.printf("Transfer: %p\n", this);

    while (!_src.done() || next_src_buffer())
      {
        /* The source data structures are already initialized, the header
           is consumed and _src stands at the very first real buffer.
           Initialize the target data structures if necessary and fill the
           header. */
        if (!_dst_head)
          {
            if (!_dst_queue->ready())
              return false;

            auto r = _dst_queue->next_avail();

            if (L4_UNLIKELY(!r))
              return false;

            _dst_head = _dst_req_proc.start(_dst_dev->mem_info(), r, &_dst);

            if (!_dst_header)
              {
                if (_dst.left < sizeof(Virtio_net::Hdr))
                  throw L4::Runtime_error(-L4_EINVAL,
                                          "Target buffer too small for header");
                _dst_header = reinterpret_cast<Virtio_net::Hdr *>(_dst.pos);
                trace.printf("\t: Copying header to %p (size: %u)\n",
                             _dst.pos, _dst.left);
                /*
                 * Header and csum offloading/general segmentation offloading
                 *
                 * We just copy the original header from source to
                 * destination and have to consider three different
                 * cases:
                 * - no flags are set
                 *   - we got a packet that is completely checksummed
                 *     and correctly fragmented, there is nothing to
                 *     do other then copying.
                 * - virtio_net_hdr_f_needs_csum set
                 *  - the packet is partially checksummed; if we would
                 *     send the packet out on the wire we would have
                 *     to calculate checksums now. But here we rely on
                 *     the ability of our guest to handle partially
                 *     checksummed packets and simply delegate the
                 *     checksum calculation to them.
                 * - gso_type != gso_none
                 *  - the packet needs to be segmented; if we would
                 *     send it out on the wire we would have to
                 *     segment it now. But again we rely on the
                 *     ability of our guest to handle gso
                 *
                 * We currently assume that our guests negotiated
                 * virtio_net_f_guest_*, this needs to be checked in
                 * the future.
                 *
                 * We also discussed the usage of
                 * virtio_net_hdr_f_data_valid to remove the need to
                 * checksum packets at all. But since our clients send
                 * partially checksummed packets anyway the only
                 * interesting case would be a packet without
                 * net_hdr_f_needs_checksum set. In that case we would
                 * signal that we checked the checksum and the
                 * checksum is actually correct. Since we do not know
                 * the origin of the packet (it could have been send
                 * by an external node and could have been routed to
                 * u) we can not signal this without actually
                 * verifying the checksum. Otherwise a packet with an
                 * invalid checksum could be successfully delivered.
                 */
                _total = sizeof(Virtio_net::Hdr);
                memcpy(_dst_header, _request->header(), _total);
                _mangle.rewrite_hdr(_dst_header);
                _dst.skip(_total);
              }
            ++_num_merged;
          }

        if (!_dst.done() || next_dst_buffer())
          {
            trace.printf("\t: Copying %p#%p:%u (%x) -> %p#%p:%u  (%x)\n",
                         _request->dev(),
                         _src.pos, _src.left, _src.left,
                         _dst_dev, _dst.pos, _dst.left, _dst.left);

            _total += _mangle.copy_pkt(_dst, _src);
          }
        else
          {
            // save descriptor information for later
            trace.printf("\t: Saving descriptor for later\n");
            _consumed.push_back(Consumed_entry(_dst_head, _total));
            _total = 0;
            _dst_head = L4virtio::Svr::Virtqueue::Head_desc();
          }
      }
    finish_transfer();
    return true;
  }

  /**
   * Finalize the Request delivery.
   *
   * This function calls `finish()` on the destination port's receive queue,
   * which will result in triggering the destination client IRQ.
   */
  void finish_transfer()
  {
    Dbg trace(Dbg::Request, Dbg::Trace, "REQ");
    if (!_dst_header)
      {
        if (!_total)
          trace.printf("\tTransfer %p - not started yet, dropping\n", this);
        return;
      }

    if (_consumed.empty())
      {
        assert(_dst_head);
        assert(_num_merged == 1);
        trace.printf("\tTransfer %p - Invoke dst_queue->finish()\n", this);
        _dst_header->num_buffers = 1;
        _dst_queue->finish(_dst_head, _dst_dev, _total);
      }
    else
      {
        assert(_dst_head);
        _dst_header->num_buffers = _num_merged;
        _consumed.push_back(Consumed_entry(_dst_head, _total));
        trace.printf("\tTransfer %p - Invoke dst_queue->finish(iter)\n", this);
        _dst_queue->finish(_consumed.begin(), _consumed.end(), _dst_dev);
      }
    _dst_header = nullptr;
  }

  ~Virtio_net_transfer()
  {
    /*
     * We ended up here after an exception or a timeout, so the
     * transfer is unfinished or has failed
     */
    finish_transfer();
  }
};
/**\}*/
