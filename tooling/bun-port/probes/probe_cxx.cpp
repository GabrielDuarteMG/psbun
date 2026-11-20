// PS5Term - probe_cxx: C++ exceptions, std::thread and thread_local objects with destructors.
// GPL-3.0-or-later.
#include <atomic>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "probe.h"

#define NAME "probe_cxx"

static std::atomic<int> destroyed{0};

struct Tracker
{
    int value = 0;
    ~Tracker() { destroyed.fetch_add(1); }
};

static thread_local Tracker tracker;

static int thrower(int depth)
{
    if (depth == 0)
        throw std::runtime_error("deep");
    return thrower(depth - 1) + 1;
}

int main()
{
    PROBE_START(NAME);
    try
    {
        thrower(20);
        return PROBE_FAIL(NAME, "no exception", EINVAL);
    }
    catch (const std::runtime_error &error)
    {
        if (std::string(error.what()) != "deep")
            return PROBE_FAIL(NAME, "exception message", EINVAL);
    }
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int i = 0; i < 4; i++)
        threads.emplace_back([i, &ok] {
            tracker.value = i + 1;
            try
            {
                throw i;
            }
            catch (int caught)
            {
                if (caught == i && tracker.value == i + 1)
                    ok.fetch_add(1);
            }
        });
    for (auto &thread : threads)
        thread.join();
    if (ok.load() != 4)
        return PROBE_FAIL(NAME, "exceptions in threads", EINVAL);
    // Diagnostics: do plain pthread key destructors run, and do thread_local ones run late?
    static std::atomic<int> key_destroyed{0};
    pthread_key_t key;
    pthread_key_create(&key, [](void *) { key_destroyed.fetch_add(1); });
    std::thread keyed([key] { pthread_setspecific(key, (void *)1); });
    keyed.join();
    usleep(200000);
    PROBE_LOG(NAME, "pthread key destructor ran %d times (want 1); thread_local %d (want 4)",
              key_destroyed.load(), destroyed.load());
    if (destroyed.load() != 4)
    {
        return PROBE_FAIL(NAME, "thread_local destructor", EINVAL);
    }
    PROBE_LOG(NAME, "exceptions, std::thread and thread_local destructors ok");
    return PROBE_PASS(NAME);
}
