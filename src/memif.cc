#define NAPI_VERSION 6
#define NAPI_DISABLE_CPP_EXCEPTIONS

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include <napi.h>
#include <uv.h>
extern "C" {
#include <libmemif.h>
}

class Memif : public Napi::ObjectWrap<Memif> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    auto func = DefineClass(env, "Memif",
                            {
                              InstanceAccessor("counters", &Memif::counters, nullptr),
                              InstanceMethod<&Memif::send>("send"),
                              InstanceMethod<&Memif::close>("close"),
                            });
    auto ctor = new Napi::FunctionReference(Napi::Persistent(func));
    env.SetInstanceData(ctor);

    exports.Set("Memif", func);
    return exports;
  }

  explicit Memif(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<Memif>(info) {
    auto env = info.Env();

    auto args = info[0].ToObject();
    auto socketName = args.Get("socketName").As<Napi::String>().Utf8Value();
    auto id = args.Get("id").As<Napi::Number>().Uint32Value();
    auto dataroom = args.Get("dataroom").As<Napi::Number>().Uint32Value();
    auto ringCapacityLog2 = args.Get("ringCapacityLog2").As<Napi::Number>().Uint32Value();
    auto isServer = args.Get("isServer").As<Napi::Boolean>().Value();
    auto rx = args.Get("rx").As<Napi::Function>();
    auto state = args.Get("state").As<Napi::Function>();
    if (env.IsExceptionPending()) {
      return;
    }

    memif_socket_args_t sa{};
    std::strncpy(sa.path, socketName.data(), sizeof(sa.path) - 1);
    std::strncpy(sa.app_name, "node-memif", sizeof(sa.app_name) - 1);
    sa.connection_request_timer.it_interval.tv_nsec = 250000000;
    sa.connection_request_timer.it_value.tv_nsec = 250000000;
    sa.on_control_fd_update = handleControlFdUpdate;
    int err = memif_create_socket(&m_sock, &sa, this);
    if (err != MEMIF_ERR_SUCCESS) {
      stop();
      NAPI_THROW_VOID(Napi::Error::New(env, "memif_create_socket error " + std::to_string(err)));
    }

    memif_conn_args_t ca{};
    ca.socket = m_sock;
    ca.interface_id = id;
    ca.buffer_size = static_cast<uint16_t>(dataroom);
    ca.log2_ring_size = static_cast<uint8_t>(ringCapacityLog2);
    ca.is_master = static_cast<uint8_t>(isServer);
    err = memif_create(&m_conn, &ca, handleConnect, handleDisconnect, handleInterrupt, this);
    if (err != MEMIF_ERR_SUCCESS) {
      stop();
      NAPI_THROW_VOID(Napi::Error::New(env, "memif_create error " + std::to_string(err)));
    }

    m_rx = Napi::Persistent(rx);
    m_state = Napi::Persistent(state);
    m_dataroom = dataroom;
    m_running = true;
  }

  static Napi::Value CreateNewItem(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto ctor = env.GetInstanceData<Napi::FunctionReference>();
    return ctor->New({info[0]});
  }

  void Finalize(Napi::Env env) override {
    stop();
  }

private:
  Napi::Value counters(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto cnt = Napi::Object::New(env);
    cnt.Set("nRxPackets", Napi::BigInt::New(env, m_nRxPackets));
    cnt.Set("nRxFragments", Napi::BigInt::New(env, m_nRxFragments));
    cnt.Set("nTxPackets", Napi::BigInt::New(env, m_nTxPackets));
    cnt.Set("nTxFragments", Napi::BigInt::New(env, m_nTxFragments));
    cnt.Set("nTxDropped", Napi::BigInt::New(env, m_nTxDropped));
    return cnt;
  }

  void send(const Napi::CallbackInfo& info) {
    auto buffer = info[0].As<Napi::ArrayBuffer>();
    uint32_t offset = info[1].ToNumber();
    uint32_t length = info[2].ToNumber();

    if (!m_connected) {
      ++m_nTxDropped;
      return;
    }

    std::vector<memif_buffer_t> b(length / m_dataroom + 1);
    uint16_t nAlloc = 0;
    int err = memif_buffer_alloc(m_conn, 0, b.data(), 1, &nAlloc, length);
    if (err != MEMIF_ERR_SUCCESS) {
      ++m_nTxDropped;
      return;
    }

    const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.Data()) + offset;
    for (uint16_t i = 0; i < nAlloc; ++i) {
      std::copy_n(data, b[i].len, reinterpret_cast<uint8_t*>(b[i].data));
      data += b[i].len;
    }

    uint16_t nTx = 0;
    err = memif_tx_burst(m_conn, 0, b.data(), nAlloc, &nTx);
    if (err != MEMIF_ERR_SUCCESS) {
      ++m_nTxDropped;
      return;
    }

    ++m_nTxPackets;
    m_nTxFragments += nAlloc;
  }

  void close(const Napi::CallbackInfo& info) {
    stop();
  }

  static int handleControlFdUpdate(memif_fd_event_t fde, void* self0) {
    auto self = reinterpret_cast<Memif*>(self0);
    if ((fde.type & MEMIF_FD_EVENT_DEL) != 0) {
      self->m_uvPolls.erase(fde.fd);
      return 0;
    }

    int uvEvents = 0;
    if ((fde.type & MEMIF_FD_EVENT_READ) != 0) {
      uvEvents |= UV_READABLE;
    }
    if ((fde.type & MEMIF_FD_EVENT_WRITE) != 0) {
      uvEvents |= UV_WRITABLE;
    }
    if (uvEvents == 0) {
      return 0;
    }

    auto& ptr = self->m_uvPolls[fde.fd];
    if (ptr == nullptr) {
      ptr.reset(new UvPoll(fde, self));
    }
    return uv_poll_start(&ptr->handle, uvEvents, handlePoll);
  }

  static void handlePoll(uv_poll_t* handle, int status, int events) {
    UvPoll& poll = UvPoll::of(handle);
    int memifEvents = 0;
    if (status < 0) {
      memifEvents = MEMIF_FD_EVENT_ERROR;
    } else {
      if ((events & UV_READABLE) != 0) {
        memifEvents |= MEMIF_FD_EVENT_READ;
      }
      if ((events & UV_WRITABLE) != 0) {
        memifEvents |= MEMIF_FD_EVENT_WRITE;
      }
    }
    memif_control_fd_handler(poll.private_ctx, static_cast<memif_fd_event_type_t>(memifEvents));
  }

  static int handleConnect(memif_conn_handle_t conn, void* self0) {
    auto self = reinterpret_cast<Memif*>(self0);
    memif_refill_queue(conn, 0, -1, 0);
    self->setState(true);
    return 0;
  }

  static int handleDisconnect(memif_conn_handle_t conn, void* self0) {
    auto self = reinterpret_cast<Memif*>(self0);
    self->setState(false);
    return 0;
  }

  static int handleInterrupt(memif_conn_handle_t conn, void* self0, uint16_t qid) {
    auto self = reinterpret_cast<Memif*>(self0);
    if (!self->m_running) {
      return 0;
    }

    std::array<memif_buffer_t, 64> burst{};
    uint16_t nRx = 0;
    int err = memif_rx_burst(conn, 0, burst.data(), burst.size(), &nRx);
    if (err == MEMIF_ERR_SUCCESS) {
      for (uint16_t i = 0; i < nRx; ++i) {
        self->receive(burst[i]);
      }
    }
    memif_refill_queue(conn, 0, nRx, 0);
    return 0;
  }

  void receive(const memif_buffer_t& b) {
    auto env = Env();
    Napi::HandleScope scope(env);
    auto u8 = Napi::Uint8Array::New(env, b.len);
    std::memcpy(u8.Data(), b.data, b.len);
    bool hasNext = (b.flags & MEMIF_BUFFER_FLAG_NEXT) != 0;
    auto hasNextB = Napi::Boolean::New(env, hasNext);
    m_rx.Call({u8, hasNextB});

    if (!hasNext) {
      ++m_nRxPackets;
    }
    ++m_nRxFragments;
  }

  void setState(bool up) {
    if (m_connected == up) {
      return;
    }
    m_connected = up;

    auto env = Env();
    Napi::HandleScope scope(env);
    m_state.Call({Napi::Boolean::New(env, up)});
  }

  void stop() {
    if (m_running) {
      m_running = false;
      setState(false);
      m_rx.Unref();
      m_state.Unref();
    }

    if (m_conn != nullptr) {
      memif_delete(&m_conn);
    }

    if (m_sock != nullptr) {
      memif_delete_socket(&m_sock);
    }

    std::vector<int> fds;
    for (const auto& entry : m_uvPolls) {
      fds.push_back(entry.second->fd);
    }
    m_uvPolls.clear();
    for (int fd : fds) {
      ::close(fd);
    }
  }

private:
  class UvPoll : public memif_fd_event_t {
  public:
    explicit UvPoll(memif_fd_event_t fde, Memif* owner)
      : memif_fd_event_t(fde)
      , owner(owner) {
      struct uv_loop_s* loop = nullptr;
      napi_get_uv_event_loop(owner->Env(), &loop);
      uv_poll_init(loop, &handle, fd);
      handle.data = this;
    }

    ~UvPoll() {
      uv_poll_stop(&handle);
    }

    static UvPoll& of(uv_poll_t* handle) {
      return *reinterpret_cast<UvPoll*>(handle->data);
    }

  public:
    uv_poll_t handle;
    Memif* owner;
  };

  std::unordered_map<int, std::unique_ptr<UvPoll>> m_uvPolls;
  memif_socket_handle_t m_sock = nullptr;
  memif_conn_handle_t m_conn = nullptr;

  Napi::FunctionReference m_rx;
  Napi::FunctionReference m_state;
  size_t m_dataroom = 0;

  uint64_t m_nRxPackets = 0;
  uint64_t m_nRxFragments = 0;
  uint64_t m_nTxPackets = 0;
  uint64_t m_nTxFragments = 0;
  uint64_t m_nTxDropped = 0;
  bool m_running = false;
  bool m_connected = false;
};

Napi::Object
Init(Napi::Env env, Napi::Object exports) {
  return Memif::Init(env, exports);
}

NODE_API_MODULE(addon, Init)
