#include <cstdio>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <vector>


#define _LOG_SRC "MAIN"
#include "log.h"



static
const log_src_descr_t log_srcs[] = {
        {"MAIN",LL_INFO },
        {"OLOLO", LL_INFO }
};

class Barrier {
public:
    explicit Barrier(unsigned int thr_count) {
        if (pthread_barrier_init(&m_b, nullptr, thr_count) != 0) {
            throw std::runtime_error("pthread_barrier_init() failed");
        }
    }
    ~Barrier() {
        pthread_barrier_destroy(&m_b);
    }
    void Wait() {
        pthread_barrier_wait(&m_b);
    }
private:
    pthread_barrier_t m_b{};
};


class Locker {
public:
    virtual void Lock() = 0;
    virtual void Unlock() = 0;
    virtual ~Locker() = default;
};

class MutexLocker : public Locker {
public:
    MutexLocker() {
        if (pthread_mutex_init(&m_mutex, nullptr) != 0) {
            throw std::runtime_error("pthread_mutex_init() failed");
        }
    }
    ~MutexLocker() override {
        pthread_mutex_destroy(&m_mutex);
    }
    void Lock() override {
        if (pthread_mutex_lock(&m_mutex) != 0) {
            throw std::runtime_error("pthread_mutex_lock() failed");
        }
    }
    void Unlock() override {
        if (pthread_mutex_unlock(&m_mutex) != 0) {
            throw std::runtime_error("pthread_mutex_unlock() failed");
        }
    }
private:
    pthread_mutex_t m_mutex{};
};

class RWLocker : public Locker {
public:
    RWLocker() {
        if (pthread_rwlock_init(&m_rw_lock, nullptr) != 0) {
            throw std::runtime_error("pthread_rwlock_init() failed");
        }
    }
    ~RWLocker() override {
        pthread_rwlock_destroy(&m_rw_lock);
    }

    void Unlock() override {
        if (pthread_rwlock_unlock(&m_rw_lock) != 0) {
            throw std::runtime_error("pthread_mutex_unlock() failed");
        }
    }
protected:
    pthread_rwlock_t m_rw_lock{};
};

class RDLocker : public RWLocker {
public:
    void Lock() override {
        if (pthread_rwlock_rdlock(&m_rw_lock) != 0) {
            throw std::runtime_error("pthread_mutex_lock() failed");
        }
    }

};

class WRLocker : public RWLocker {
public:
    void Lock() override {
        if (pthread_rwlock_wrlock(&m_rw_lock) != 0) {
            throw std::runtime_error("pthread_mutex_lock() failed");
        }
    }
};

// spin lock бесполнезен на однопроцессорных (одноядерных) системах.
#ifdef USE_SPINLOCK
class SpinLocker : public Locker {
public:
    SpinLocker() {
        if (pthread_spin_init(&m_spinlock, PTHREAD_PROCESS_PRIVATE) != 0) {
            throw std::runtime_error("pthread_spin_init() failed");
        }
    }
    ~SpinLocker() override {
        pthread_spin_destroy(&m_spinlock);
    }
    void Lock() override {
        if (pthread_spin_lock(&m_spinlock) != 0) {
            throw std::runtime_error("pthread_spin_lock() failed");
        }
    }
    void Unlock() override {
        if (pthread_spin_unlock(&m_spinlock) != 0) {
            throw std::runtime_error("pthread_spin_unlock() failed");
        }
    }
private:
    pthread_spinlock_t m_spinlock{};
};
#endif

struct Sync {
    Locker       *locker;
    Barrier      *barrier1;
    Barrier      *barrier2;
    unsigned int  lock_count;
};

static
void *thread_routine(void *arg) {
    Sync *s = static_cast<Sync*>(arg);
    unsigned int lock_count = s->lock_count;
    s->barrier1->Wait();
    for (unsigned int i = 0; i < lock_count; i++) {
        s->locker->Lock();
        s->locker->Unlock();
    }
    s->barrier2->Wait();
    return nullptr;
}

class Worker {
public:
    Worker(Locker *locker, Barrier *barrier1, Barrier *barrier2, unsigned int lock_count)
    : m_sync{.locker = locker, .barrier1 = barrier1, .barrier2 = barrier2, .lock_count = lock_count}, m_thread(0) {
        if (pthread_create(&m_thread, nullptr, thread_routine, &m_sync) != 0) {
            throw std::runtime_error("pthread_create() failed");
        }
        _LOG_TRACE("thr %lu created", m_thread);
    }
    Worker(Worker && other) noexcept {
        this->m_sync = other.m_sync;
        this->m_thread = other.m_thread;
        other.m_sync = {};
        other.m_thread = 0;
    }
    ~Worker() {
        if (m_thread) {
            int err = pthread_join(m_thread, nullptr);
            if (err != 0) {
                _LOG_ERROR_ERRNO2("pthread_join failed()", err);
            }
            _LOG_TRACE("thr %lu destroyed", m_thread);
        }
    }
private:
    Sync      m_sync{};
    pthread_t m_thread;

};

static
void process(Locker       *locker,
             unsigned int  thr_count,
             unsigned int  lock_count) {
    Barrier barrier1(thr_count+1);
    Barrier barrier2(thr_count+1);
    std::vector<std::unique_ptr<Worker>> workers;
    for (size_t i = 0; i < thr_count; i++) {
        workers.push_back(std::make_unique<Worker>(locker, &barrier1, &barrier2, lock_count));
    }
    barrier1.Wait();
    // в этой точке все рабочие потоки запущены и начали вхождение в критическую секцию
    const auto start = std::chrono::steady_clock::now();
    _LOG_INFO("Started with %u concurrent threads. Each thread enter critical section %u times", thr_count, lock_count);
    barrier2.Wait();
    // в этой точке все рабочие потоки завершили операции с критическую секцию
    const auto end = std::chrono::steady_clock::now();
    _LOG_INFO("Finished. Elapsed time = %zu ms",
              std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

static
bool str_to_uint(const char   *str,
                 unsigned int *val,
                 const char   *val_name) {
    if (sscanf(str, "%u", val) == 1) {
        return true;
    }
    _LOG_ERROR("bad value of %s: %s", val_name, str);
    return false;
}

static
Locker *create_locker(const char *type) {
    std::string type_str(type);
    std::transform(type_str.begin(), type_str.end(), type_str.begin(), ::toupper);
    if (type_str == "MUTEX") {
        return new MutexLocker();
    }
    if (type_str == "RD_LOCK") {
        return new RDLocker();
    }
    if (type_str == "WR_LOCK") {
        return new WRLocker();
    }
#ifdef USE_SPINLOCK
    if (type_str == "SPIN_LOCK") {
        return new WRLocker();
    }
#endif
    return nullptr;
}

class Logger {
public:
    Logger() {
        log_init(LL_TRACE, false);
        log_register_ex(log_srcs, TBL_SZ(log_srcs));
    }
    ~Logger() {
        log_destroy();
    }
};

int main(int argc, const char **argv) {
    Logger l;
    if (argc < 4) {
        _LOG_ERROR("thread count and/or lock count and/or locker_type are not specified");
        puts("Usage: mutex_vs_rwlock thread_count lock_count mutex|rd_lock|wr_lock");
        return -1;
    }
    unsigned int thr_count = 0;
    unsigned int lock_count = 0;
    if (!str_to_uint(argv[1], &thr_count, "thr_count")) {
        return -2;
    }
    if (!str_to_uint(argv[2], &lock_count, "lock_count")) {
        return -2;
    }
    std::unique_ptr<Locker> locker(create_locker(argv[3]));
    if (!locker) {
        _LOG_ERROR("unknown locker type");
        return -3;
    }
    _LOG_INFO("Hellow!");
    process(locker.get(), thr_count, lock_count);
    _LOG_INFO("Bye!");
    return 0;
}