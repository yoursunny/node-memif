#define NAPI_VERSION 6
#define NAPI_DISABLE_CPP_EXCEPTIONS

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include <napi.h>
#include <uv.h>
extern "C"
{
#include <libmemif.h>
}

class Memif : public Napi::ObjectWrap<Memif>
{
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports)
  {
    auto func = DefineClass(env, "Memif",
                            {
                              InstanceAccessor("counters", &Memif::counters, nullptr),
                              InstanceMethod<&Memif::send>("send"),
                              InstanceMethod<&Memif::close>("close"),
                            });
    auto ctor = new Napi::FunctionReference();
    *ctor = Napi::Persistent(func);
    env.SetInstanceData(ctor);

    exports.Set("Memif", func);
    return exports;
  }

  explicit Memif(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<Memif>(info)
  {
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

    if (s_instance != nullptr) {
      Napi::Error::Fatal(__FILE__, "cannot create multiple Memif instances");
    }
    s_instance = this;
    int err =
      memif_init(handleControlFdUpdate, const_cast<char*>("node-memif"), nullptr, nullptr, nullptr);
    if (err != MEMIF_ERR_SUCCESS) {
      s_instance = nullptr;
      NAPI_THROW_VOID(Napi::Error::New(env, "memif_init error " + std::to_string(err)));
    }

    err = memif_create_socket(&m_sock, socketName.data(), this);
    if (err != MEMIF_ERR_SUCCESS) {
      stop();
      NAPI_THROW_VOID(
        Napi::Error::New(env, "memif_per_thread_create_socket error " + std::to_string(err)));
    }

    memif_conn_args_t cargs{};
    cargs.socket = m_sock;
    cargs.interface_id = id;
    cargs.buffer_size = static_cast<uint16_t>(dataroom);
    cargs.log2_ring_size = static_cast<uint8_t>(ringCapacityLog2);
    cargs.is_master = static_cast<uint8_t>(isServer);
    err = memif_create(&m_conn, &cargs, handleConnect, handleDisconnect, handleInterrupt, this);
    if (err != MEMIF_ERR_SUCCESS) {
      stop();
      NAPI_THROW_VOID(Napi::Error::New(env, "memif_create error " + std::to_string(err)));
    }

    m_rx = Napi::Persistent(rx);
    m_state = Napi::Persistent(state);
    m_running = true;
    s_instance = this;
  }

  static Napi::Value CreateNewItem(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto ctor = env.GetInstanceData<Napi::FunctionReference>();
    auto v = info[0];
    return ctor->New({ v });
  }

  void Finalize(Napi::Env env) override
  {
    stop();
  }

private:
  Napi::Value counters(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    auto cnt = Napi::Object::New(env);
    cnt.Set("nRxDelivered", Napi::BigInt::New(env, m_nRxDelivered));
    cnt.Set("nRxDropped", Napi::BigInt::New(env, m_nRxDropped));
    cnt.Set("nTxDelivered", Napi::BigInt::New(env, m_nTxDelivered));
    cnt.Set("nTxDropped", Napi::BigInt::New(env, m_nTxDropped));
    return cnt;
  }

  void send(const Napi::CallbackInfo& info)
  {
    if (!m_connected) {
      ++m_nTxDropped;
      return;
    }

    auto u8 = info[0].As<Napi::Uint8Array>();
    size_t len = u8.ByteLength();

    memif_buffer_t b{};
    uint16_t nAlloc = 0;
    int err = memif_buffer_alloc(m_conn, 0, &b, 1, &nAlloc, len);
    if (err != MEMIF_ERR_SUCCESS) {
      ++m_nTxDropped;
      return;
    }
    std::copy_n(u8.Data(), len, reinterpret_cast<uint8_t*>(b.data));

    uint16_t nTx = 0;
    err = memif_tx_burst(m_conn, 0, &b, 1, &nTx);
    if (err != MEMIF_ERR_SUCCESS) {
      ++m_nTxDropped;
      return;
    }

    ++m_nTxDelivered;
  }

  void close(const Napi::CallbackInfo& info)
  {
    stop();
  }

  static int handleControlFdUpdate(int fd, uint8_t events, void*)
  {
    auto self = s_instance;
    if ((events & MEMIF_FD_EVENT_DEL) != 0) {
      self->m_uvPolls.erase(fd);
      return 0;
    }

    int uvEvents = 0;
    if ((events & MEMIF_FD_EVENT_READ) != 0) {
      uvEvents |= UV_READABLE;
    }
    if ((events & MEMIF_FD_EVENT_WRITE) != 0) {
      uvEvents |= UV_WRITABLE;
    }
    if (uvEvents == 0) {
      return 0;
    }

    auto& ptr = self->m_uvPolls[fd];
    if (ptr == nullptr) {
      ptr.reset(new UvPoll(self, fd));
    }
    return uv_poll_start(&ptr->handle, uvEvents, handlePoll);
  }

  static void handlePoll(uv_poll_t* handle, int status, int events)
  {
    UvPoll& poll = UvPoll::of(handle);
    uint8_t memifEvents = 0;
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

    Napi::HandleScope scope(poll.owner->Env());
    memif_control_fd_handler(poll.fd, memifEvents);
  }

  static int handleConnect(memif_conn_handle_t conn, void* self0)
  {
    auto self = reinterpret_cast<Memif*>(self0);
    memif_refill_queue(conn, 0, -1, 0);
    self->setState(true);
    return 0;
  }

  static int handleDisconnect(memif_conn_handle_t conn, void* self0)
  {
    auto self = reinterpret_cast<Memif*>(self0);
    self->setState(false);
    return 0;
  }

  static int handleInterrupt(memif_conn_handle_t conn, void* self0, uint16_t qid)
  {
    auto self = reinterpret_cast<Memif*>(self0);
    memif_buffer_t b{};
    uint16_t nRx = 0;
    int err = memif_rx_burst(conn, 0, &b, 1, &nRx);
    if (err == MEMIF_ERR_SUCCESS && nRx == 1) {
      auto u8 = Napi::Uint8Array::New(self->Env(), b.len);
      std::memcpy(u8.Data(), b.data, b.len);
      self->m_rx.Call({ u8 });
      ++self->m_nRxDelivered;
    }
    memif_refill_queue(conn, 0, nRx, 0);
    return 0;
  }

  void setState(bool up)
  {
    if (m_connected == up) {
      return;
    }
    m_connected = up;
    m_state.Call({ Napi::Boolean::New(Env(), up) });
  }

  void stop()
  {
    if (m_running) {
      m_running = false;
      // memif_cancel_poll_event();
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

    if (s_instance == this) {
      memif_cleanup();

      std::vector<int> fds;
      for (const auto& entry : m_uvPolls) {
        fds.push_back(entry.second->fd);
      }
      m_uvPolls.clear();
      for (int fd : fds) {
        ::close(fd);
      }

      s_instance = nullptr;
    }
  }

private:
  class UvPoll
  {
  public:
    explicit UvPoll(Memif* owner, int fd)
      : owner(owner)
      , fd(fd)
    {
      struct uv_loop_s* loop = nullptr;
      napi_get_uv_event_loop(owner->Env(), &loop);
      uv_poll_init(loop, &handle, fd);
      handle.data = this;
    }

    ~UvPoll()
    {
      uv_poll_stop(&handle);
    }

    static UvPoll& of(uv_poll_t* handle)
    {
      return *reinterpret_cast<UvPoll*>(handle->data);
    }

  public:
    uv_poll_t handle;
    Memif* owner;
    int fd;
  };

  static Memif* s_instance;
  Napi::FunctionReference m_rx;
  Napi::FunctionReference m_state;
  std::unordered_map<int, std::unique_ptr<UvPoll>> m_uvPolls;
  memif_socket_handle_t m_sock = nullptr;
  memif_conn_handle_t m_conn = nullptr;
  uint64_t m_nRxDelivered = 0;
  uint64_t m_nRxDropped = 0;
  uint64_t m_nTxDelivered = 0;
  uint64_t m_nTxDropped = 0;
  bool m_running = false;
  bool m_connected = false;
};

Memif* Memif::s_instance = nullptr;

Napi::Object
Init(Napi::Env env, Napi::Object exports)
{
  return Memif::Init(env, exports);
}

NODE_API_MODULE(addon, Init)
