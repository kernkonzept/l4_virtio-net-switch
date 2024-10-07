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
 * `Virtio_port::handle_request` uses the transfer function to move one
 * packet to the destination of the request.
 */
class Virtio_net_transfer
{
public:
  enum class Result
  {
    Delivered, Exception, Dropped,
  };

  /**
   * Deliver the request to the destination port
   *
   * \param request   The associated network request
   * \param dst_dev   The destination port
   * \param dst_queue The Receive queue of the destination port
   * \param mangle    The VLAN packet rewriting handler for the transfer
   *
   * \throws L4virtio::Svr::Bad_descriptor  Exception raised in SRC port queue.
   */
  static
  Result transfer(Virtio_net_request const &request,
                  Virtio_net *dst_dev,
                  L4virtio::Svr::Virtqueue *dst_queue,
                  Virtio_vlan_mangle &mangle)
  {
    Dbg trace(Dbg::Request, Dbg::Trace, "REQ");
    trace.printf("Transfer request: %p\n", request.header());

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
    L4virtio::Svr::Request_processor src_req_proc = request.get_request_processor();

    /* the buffer descriptors used for this transaction and the amount
     * of bytes copied to the current target descriptor */
    Buffer src = request.first_buffer();
    Buffer dst;
    int total = 0;
    l4_uint16_t num_merged = 0;
    typedef cxx::Pair<L4virtio::Svr::Virtqueue::Head_desc, l4_uint32_t> Consumed_entry;
    std::vector<Consumed_entry> consumed;

    L4virtio::Svr::Virtqueue::Head_desc dst_head;
    L4virtio::Svr::Request_processor dst_req_proc;
    Virtio_net::Hdr *dst_header = nullptr;

    for (;;)
      {
        try
          {
            if (src.done() && !src_req_proc.next(request.dev()->mem_info(), &src))
              // Request completely copied to destination.
              break;
          }
        catch (L4virtio::Svr::Bad_descriptor &e)
          {
            trace.printf("\tTransfer failed, bad descriptor exception, dropping.\n");

            // Handle partial transfers to destination port.
            if (!consumed.empty())
              // Partial transfer, rewind to before first descriptor of transfer.
              dst_queue->rewind_avail(consumed.at(0).first);
            else if (dst_head)
              // Partial transfer, still at first _dst_head.
              dst_queue->rewind_avail(dst_head);
            throw;
          }

        /* The source data structures are already initialized, the header
           is consumed and src stands at the very first real buffer.
           Initialize the target data structures if necessary and fill the
           header. */
        if (!dst_head)
          {
            if (!dst_queue->ready())
              return Result::Dropped;

            auto r = dst_queue->next_avail();

            if (L4_UNLIKELY(!r))
              {
                trace.printf("\tTransfer %p failed, destination queue depleted, dropping.\n",
                             request.header());
                // Abort incomplete transfer.
                if (!consumed.empty())
                  dst_queue->rewind_avail(consumed.front().first);
                return Result::Dropped;
              }

            try
              {
                dst_head = dst_req_proc.start(dst_dev->mem_info(), r, &dst);
              }
            catch (L4virtio::Svr::Bad_descriptor &e)
              {
                Dbg(Dbg::Request, Dbg::Warn, "REQ")
                  .printf("%s: bad descriptor exception: %s - %i"
                          " -- signal device error in destination device %p.\n",
                          __PRETTY_FUNCTION__, e.message(), e.error, dst_dev);

                dst_dev->device_error();
                return Result::Exception; // Must not touch the dst queues anymore.
              }

            if (!dst_header)
              {
                if (dst.left < sizeof(Virtio_net::Hdr))
                  throw L4::Runtime_error(-L4_EINVAL,
                                          "Target buffer too small for header");
                dst_header = reinterpret_cast<Virtio_net::Hdr *>(dst.pos);
                trace.printf("\t: Copying header to %p (size: %u)\n",
                             dst.pos, dst.left);
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
                total = sizeof(Virtio_net::Hdr);
                memcpy(dst_header, request.header(), total);
                mangle.rewrite_hdr(dst_header);
                dst.skip(total);
              }
            ++num_merged;
          }

        bool has_next_dst_buffer = false;
        try
          {
            has_next_dst_buffer = dst_req_proc.next(dst_dev->mem_info(), &dst);
          }
        catch (L4virtio::Svr::Bad_descriptor &e)
          {
            Dbg(Dbg::Request, Dbg::Warn, "REQ")
              .printf("%s: bad descriptor exception: %s - %i"
                      " -- signal device error in destination device %p.\n",
                      __PRETTY_FUNCTION__, e.message(), e.error, dst_dev);
            dst_dev->device_error();
            return Result::Exception; // Must not touch the dst queues anymore.
          }

        if (!dst.done() || has_next_dst_buffer)
          {
            trace.printf("\t: Copying %p#%p:%u (%x) -> %p#%p:%u  (%x)\n",
                         request.dev(),
                         src.pos, src.left, src.left,
                         dst_dev, dst.pos, dst.left, dst.left);

            total += mangle.copy_pkt(dst, src);
          }
        else
          {
            // save descriptor information for later
            trace.printf("\t: Saving descriptor for later\n");
            consumed.push_back(Consumed_entry(dst_head, total));
            total = 0;
            dst_head = L4virtio::Svr::Virtqueue::Head_desc();
          }
      }

    /*
     * Finalize the Request delivery. Call `finish()` on the destination
     * port's receive queue, which will result in triggering the destination
     * client IRQ.
     */

    if (!dst_header)
      {
        if (!total)
          trace.printf("\tTransfer %p - not started yet, dropping\n", request.header());
        return Result::Dropped;
      }

    if (consumed.empty())
      {
        assert(dst_head);
        assert(num_merged == 1);
        trace.printf("\tTransfer - Invoke dst_queue->finish()\n");
        dst_header->num_buffers = 1;
        dst_queue->finish(dst_head, dst_dev, total);
      }
    else
      {
        assert(dst_head);
        dst_header->num_buffers = num_merged;
        consumed.push_back(Consumed_entry(dst_head, total));
        trace.printf("\tTransfer - Invoke dst_queue->finish(iter)\n");
        dst_queue->finish(consumed.begin(), consumed.end(), dst_dev);
      }

    return Result::Delivered;
  }
};
/**\}*/
