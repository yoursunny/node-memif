#define NAPI_VERSION 6
#define NAPI_DISABLE_CPP_EXCEPTIONS

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

#include <napi.h>
extern "C"
{
#include <libmemif.h>
}

static void
bufferFinalize(Napi::Env env, void* value32)
{
  auto* value = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(value32) - sizeof(uint32_t));
  delete[] value;
}

static void
bufferCallback(Napi::Env env, Napi::Function fn, std::nullptr_t* ctx, uint8_t* value)
{
  auto ab = Napi::ArrayBuffer::New(env, &value[sizeof(uint32_t)],
                                   *reinterpret_cast<const uint32_t*>(value), bufferFinalize);
  auto u8 = Napi::Uint8Array::New(env, ab.ByteLength(), ab, 0);
  fn.Call({ u8 });
}

using BufferCallback = Napi::TypedThreadSafeFunction<std::nullptr_t, uint8_t, bufferCallback>;

static void
booleanCallback(Napi::Env env, Napi::Function fn, std::nullptr_t* ctx, void* b0)
{
  auto b = Napi::Boolean::New(env, reinterpret_cast<uintptr_t>(b0) != 0);
  fn.Call({ b });
}

using BooleanCallback = Napi::TypedThreadSafeFunction<std::nullptr_t, void, booleanCallback>;

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
    exports.Set("Memif", func);
    env.SetInstanceData(ctor);
    return exports;
  }

  explicit Memif(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<Memif>(info)
  {
    static std::once_flag initOnce;
    static int initErr = -1;
    std::call_once(initOnce, [&] {
      initErr = memif_init(nullptr, const_cast<char*>("node-memif"), nullptr, nullptr, nullptr);
    });

    auto env = info.Env();
    if (initErr != MEMIF_ERR_SUCCESS) {
      NAPI_THROW_VOID(Napi::Error::New(env, "memif_init error " + std::to_string(initErr)));
    }

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

    int err = memif_create_socket(&m_sock, socketName.data(), nullptr);
    if (err != MEMIF_ERR_SUCCESS) {
      NAPI_THROW_VOID(Napi::Error::New(env, "memif_create_socket error " + std::to_string(err)));
    }

    memif_conn_args_t cargs{};
    cargs.socket = m_sock;
    cargs.interface_id = id;
    cargs.buffer_size = static_cast<uint16_t>(dataroom);
    cargs.log2_ring_size = static_cast<uint8_t>(ringCapacityLog2);
    cargs.is_master = static_cast<uint8_t>(isServer);
    err = memif_create(&m_conn, &cargs, handleConnect, handleDisconnect, handleInterrupt, this);
    if (err != MEMIF_ERR_SUCCESS) {
      memif_delete_socket(&m_sock);
      NAPI_THROW_VOID(Napi::Error::New(env, "memif_create error " + std::to_string(err)));
    }

    m_rx = BufferCallback::New(env, rx, "rx", 1024, 1, nullptr);
    m_state = BooleanCallback::New(env, state, "state", 1, 1, nullptr);

    m_running = true;
    m_th = std::thread([this] { loop(); });
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
    cnt.Set("nRxDelivered",
            Napi::BigInt::New(env, m_nRxDeliveredAtomic.load(std::memory_order_relaxed)));
    cnt.Set("nRxDropped",
            Napi::BigInt::New(env, m_nRxDroppedAtomic.load(std::memory_order_relaxed)));
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
      uint8_t* copy = new uint8_t[sizeof(uint32_t) + b.len];
      *reinterpret_cast<uint32_t*>(copy) = b.len;
      std::copy_n(reinterpret_cast<const uint8_t*>(b.data), b.len, &copy[sizeof(uint32_t)]);
      if (self->m_rx.NonBlockingCall(copy) == napi_ok) {
        self->m_nRxDeliveredAtomic.store(++self->m_nRxDelivered, std::memory_order_relaxed);
      } else {
        self->m_nRxDroppedAtomic.store(++self->m_nRxDropped, std::memory_order_relaxed);
        delete[] copy;
      }
    }
    memif_refill_queue(conn, 0, nRx, 0);
    return 0;
  }

  void loop()
  {
    while (m_running) {
      memif_poll_event(1);
    }
  }

  void setState(bool up)
  {
    if (m_connected.exchange(up) == up) {
      return;
    }
    m_state.BlockingCall(reinterpret_cast<void*>(static_cast<uintptr_t>(up)));
  }

  void stop()
  {
    if (m_running.exchange(false)) {
      m_th.join();
      setState(false);
      m_rx.Release();
      m_state.Release();
    }

    if (m_conn != nullptr) {
      memif_delete(&m_conn);
    }
    if (m_sock != nullptr) {
      memif_delete_socket(&m_sock);
    }
  }

private:
  BufferCallback m_rx;
  BooleanCallback m_state;
  std::thread m_th;
  std::atomic_bool m_running{ false };
  std::atomic_bool m_connected{ false };
  std::atomic_uint64_t m_nRxDeliveredAtomic{ 0 };
  std::atomic_uint64_t m_nRxDroppedAtomic{ 0 };

  memif_socket_handle_t m_sock = nullptr;
  memif_conn_handle_t m_conn = nullptr;
  uint64_t m_nRxDelivered = 0;
  uint64_t m_nRxDropped = 0;
  uint64_t m_nTxDelivered = 0;
  uint64_t m_nTxDropped = 0;
};

Napi::Object
Init(Napi::Env env, Napi::Object exports)
{
  return Memif::Init(env, exports);
}

NODE_API_MODULE(addon, Init)
