#include <l4/re/env>
#include <l4/re/dataspace>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/virtio-net-switch/stats.h>

class Switch_statistics
{
private:
  L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap _ds;
  Virtio_net_switch::Statistics *_stats;
  bool _initialized = false;

  Switch_statistics() {}

  ~Switch_statistics()
  {
    if (_initialized)
      L4Re::Env::env()->rm()->detach(reinterpret_cast<l4_addr_t>(_stats), 0);
  }

  l4_size_t _size;

public:
  Virtio_net_switch::Statistics *stats()
  {
    if (_initialized)
      return _stats;
    else
      throw L4::Runtime_error(-L4_EAGAIN, "Statistics not set up.");
  }

  static Switch_statistics& get_instance()
  {
    static Switch_statistics instance;
    return instance;
  }

  void initialize(l4_uint64_t num_max_ports)
  {
    _size = l4_round_page(sizeof(Virtio_net_switch::Statistics)
                          + sizeof(Virtio_net_switch::Port_statistics) * num_max_ports);
    void *addr = malloc(_size);
    if (!addr)
      throw L4::Runtime_error(-L4_ENOMEM,
                              "Could not allocate statistics memory.");

    memset(addr, 0, _size);
    _stats = reinterpret_cast<Virtio_net_switch::Statistics *>(addr);
    _initialized = true;
    _stats->max_ports = num_max_ports;
  }

  Virtio_net_switch::Port_statistics *
  allocate_port_statistics(char const* name)
  {
    for (unsigned i = 0; i < _stats->max_ports; ++i)
      {
        if (!_stats->port_stats[i].in_use)
          {
            memset(reinterpret_cast<void*>(&_stats->port_stats[i]), 0,
                   sizeof(Virtio_net_switch::Port_statistics));
            _stats->port_stats[i].in_use = 1;
            size_t len = std::min(strlen(name), sizeof(_stats->port_stats[i].name) - 1);
            memcpy(_stats->port_stats[i].name, name, len);
            _stats->port_stats[i].name[len] = '\0';
            _stats->age++;
            return &_stats->port_stats[i];
          }
      }
    return nullptr;
  }

  inline l4_size_t size()
  { return _size; }

  Switch_statistics(Switch_statistics const&) = delete;
  void operator=(Switch_statistics const &) = delete;
};
