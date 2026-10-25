#include <thread>
#include <variant>
#include <set>
#include <unordered_map>
#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#endif

// Start time for the program
static std::chrono::high_resolution_clock::time_point start_time = std::chrono::high_resolution_clock::now();
// Offset of the duration since program start used to calculate the value for osGetTime. 
static int64_t ostime_offset = 0;
// Game speed multiplier (1 means no speedup)
constexpr uint32_t speed_multiplier = 1;
// N64 CPU counter ticks per millisecond
constexpr uint32_t counter_per_ms = 46'875 * speed_multiplier;

#if defined(__EMSCRIPTEN__)
// The timer thread's queue, whose wait_dequeue_timed ends at its time. On a page a timed wait can end long after its
// time: in Firefox on Windows every one ends on the system timer's tick, 15.6 ms unless the browser's process asks for
// a finer one, so a timer a fraction of a millisecond away fired a tick late, and the game thread waiting for it waited
// that long (Conker's timers are 0.4 ms or less, about 60 a second). So a timer less than 2 ms away is waited for by
// polling the queue, and a farther one on a timer as before: polling the last stretch before those too, up to a tick,
// kept the browser ~0.6 cores busier in Firefox with Banjo's page and presented no more.
template<typename T>
struct TimerQueue : moodycamel::BlockingConcurrentQueue<T> {
    TimerQueue() : moodycamel::BlockingConcurrentQueue<T>() {}

    template<typename Rep, typename Period>
    bool wait_dequeue_timed(T& item, const std::chrono::duration<Rep, Period>& timeout) {
        if (timeout >= std::chrono::milliseconds(2)) {
            return moodycamel::BlockingConcurrentQueue<T>::wait_dequeue_timed(item, timeout);
        }

        const auto deadline = std::chrono::high_resolution_clock::now() + timeout;
        while (std::chrono::high_resolution_clock::now() < deadline) {
            if (this->try_dequeue(item)) {
                return true;
            }
        }

        return false;
    }
};
#else
template<typename T>
using TimerQueue = moodycamel::BlockingConcurrentQueue<T>;
#endif

struct OSTimer {
    PTR(OSTimer) unused1;
    PTR(OSTimer) unused2;
    OSTime interval;
    OSTime timestamp;
    PTR(OSMesgQueue) mq;
    OSMesg msg;
};

struct AddTimerAction {
    PTR(OSTimer) timer;
};

struct RemoveTimerAction {
    PTR(OSTimer) timer;
};

using Action = std::variant<AddTimerAction, RemoveTimerAction>;

struct {
    std::thread thread;
    TimerQueue<Action> action_queue{};
} timer_context;

uint64_t duration_to_ticks(std::chrono::high_resolution_clock::duration duration) {
    uint64_t delta_micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    // More accurate than using a floating point timer, will only overflow after running for 12.47 years
    // Units: (micros * (counts/millis)) / (micros/millis) = counts
    uint64_t total_count = (delta_micros * counter_per_ms) / 1000;

    return total_count;
}

std::chrono::microseconds ticks_to_duration(uint64_t ticks) {
    using namespace std::chrono_literals;
    return ticks * 1000us / counter_per_ms;
}

std::chrono::high_resolution_clock::time_point ticks_to_timepoint(uint64_t ticks) {
    return start_time + ticks_to_duration(ticks);
}

uint64_t time_now() {
    return duration_to_ticks(std::chrono::high_resolution_clock::now() - start_time);
}

#if defined(__EMSCRIPTEN__)
// The timer thread's active timers, ordered by a copy of each one's timestamp taken as it is inserted, not by the
// timestamp in the OSTimer: osSetTimer writes that on the game's thread as it is called, and a game that restarts a
// timer still pending (osStopTimer, then osSetTimer, as Banjo-Tooie's scheduler does at each retrace with two timers)
// moved it inside a set ordered by it while it was there. Out of order, the set's erase missed it, so the timer stayed
// first and fired at every pass with no wait, its messages held for a full queue (ULTRAMODERN_HOLD_BLOCKED_MESSAGES)
// faster than the game took them: the game thread receiving them never blocked again, the threads below it never ran,
// and the held messages took the heap at about 13 MB a second to Aborted(OOM). A timer is still pending at its restart
// only where the timer thread runs late, which on a phone's slow cores it does at a level change. It has the calls the
// timer thread makes on a std::set, so the thread's loop is the same either way.
class TimerSet {
public:
    TimerSet(RDRAM_ARG1) : rdram(rdram) {}

    bool empty() const {
        return timers.empty();
    }

    // The first entry, the timer closest to running out.
    auto begin() const {
        return timers.begin();
    }

    // Adds a timer at its timestamp now, in place of where it was if it is active already.
    void insert(PTR(OSTimer) timer_) {
        erase(timer_);
        OSTime timestamp = TO_PTR(OSTimer, timer_)->timestamp;
        timers.insert(Entry{ timestamp, timer_ });
        keys[timer_] = timestamp;
    }

    void erase(PTR(OSTimer) timer_) {
        auto found = keys.find(timer_);
        if (found != keys.end()) {
            timers.erase(Entry{ found->second, timer_ });
            keys.erase(found);
        }
    }

private:
    // An entry reads as its timer, as a std::set<PTR(OSTimer)>'s does.
    struct Entry {
        OSTime timestamp;
        PTR(OSTimer) timer;

        operator PTR(OSTimer)() const {
            return timer;
        }

        bool operator<(const Entry& other) const {
            return timestamp != other.timestamp ? timestamp < other.timestamp : timer < other.timer;
        }
    };

    uint8_t* rdram;
    std::set<Entry> timers{};
    std::unordered_map<PTR(OSTimer), OSTime> keys{};
};
#endif

void timer_thread(RDRAM_ARG1) {
    ultramodern::set_native_thread_name("Timer Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::VeryHigh);

#if defined(__EMSCRIPTEN__)
    TimerSet active_timers{PASS_RDRAM1};
#else
    // Lambda comparator function to keep the set ordered
    auto timer_sort = [PASS_RDRAM1](PTR(OSTimer) a_, PTR(OSTimer) b_) {
        OSTimer* a = TO_PTR(OSTimer, a_);
        OSTimer* b = TO_PTR(OSTimer, b_);

        // Order by timestamp if the timers have different timestamps
        if (a->timestamp != b->timestamp) {
            return a->timestamp < b->timestamp;
        }

        // If they have the exact same timestamp then order by address instead
        return a < b;
    };

    // Ordered set of timers that are currently active
    std::set<PTR(OSTimer), decltype(timer_sort)> active_timers{timer_sort};
#endif
    
    // Lambda to process a timer action to handle adding and removing timers
    auto process_timer_action = [&](const Action& action) {
        // Determine the action type and act on it
        if (const auto* add_action = std::get_if<AddTimerAction>(&action)) {
            active_timers.insert(add_action->timer);
        } else if (const auto* remove_action = std::get_if<RemoveTimerAction>(&action)) {
            active_timers.erase(remove_action->timer);
        }
    };

    while (true) {
        // Empty the action queue
        Action cur_action;
        while (timer_context.action_queue.try_dequeue(cur_action)) {
            process_timer_action(cur_action);
        }

        // If there's no timer to act on, wait for one to come in from the action queue
        while (active_timers.empty()) {
            timer_context.action_queue.wait_dequeue(cur_action);
            process_timer_action(cur_action);
        }

        // Get the timer that's closest to running out
        PTR(OSTimer) cur_timer_ = *active_timers.begin();
        OSTimer* cur_timer = TO_PTR(OSTimer, cur_timer_);

        // Remove the timer from the queue (it may get readded if waiting is interrupted)
        active_timers.erase(cur_timer_);

        // Determine how long to wait to reach the timer's timestamp
        auto wait_duration = ticks_to_timepoint(cur_timer->timestamp) - std::chrono::high_resolution_clock::now();

        // Wait for either the duration to complete or a new action to come through
        if (wait_duration.count() >= 0 && timer_context.action_queue.wait_dequeue_timed(cur_action, wait_duration)) {
            // Timer was interrupted by a new action 
            // Add the current timer back to the queue (done first in case the action is to remove this timer)
            active_timers.insert(cur_timer_);
            // Process the new action
            process_timer_action(cur_action);
        }
        else {
            // Waiting for the timer completed, so send the timer's message to its message queue
            ultramodern::enqueue_external_message_src(cur_timer->mq, cur_timer->msg, false, ultramodern::EventMessageSource::Timer);
            // If the timer has a specified interval then reload it with that value
            if (cur_timer->interval != 0) {
                cur_timer->timestamp = cur_timer->interval + time_now();
                active_timers.insert(cur_timer_);
            }
        }
    }
}

void ultramodern::init_timers(RDRAM_ARG1) {
    timer_context.thread = std::thread{ timer_thread, PASS_RDRAM1 };
    timer_context.thread.detach();
}

uint32_t ultramodern::get_speed_multiplier() {
    return speed_multiplier;
}

std::chrono::high_resolution_clock::time_point ultramodern::get_start() {
    return start_time;
}

std::chrono::high_resolution_clock::duration ultramodern::time_since_start() {
    return std::chrono::high_resolution_clock::now() - start_time;
}

extern "C" u32 osGetCount() {
    uint64_t total_count = time_now();

    // Allow for overflows, which is how osGetCount behaves
    return (uint32_t)total_count;
}

extern "C" void osSetCount(u32 count) {
    assert(false);
}

extern "C" OSTime osGetTime() {
    uint64_t total_count = time_now() - ostime_offset;

    return total_count;
}

extern "C" void osSetTime(OSTime t) {
    ostime_offset = time_now() - t;
}

extern "C" int osSetTimer(RDRAM_ARG PTR(OSTimer) t_, OSTime countdown, OSTime interval, PTR(OSMesgQueue) mq, OSMesg msg) {
    OSTimer* t = TO_PTR(OSTimer, t_);

    // Determine the time when this timer will trigger off
    if (countdown == 0) {
        // Set the timestamp based on the interval
        t->timestamp = interval + time_now();
    } else {
        t->timestamp = countdown + time_now();
    }
    t->interval = interval;
    t->mq = mq;
    t->msg = msg;

    timer_context.action_queue.enqueue(AddTimerAction{ t_ });

    return 0;
}

extern "C" int osStopTimer(RDRAM_ARG PTR(OSTimer) t_) {
    timer_context.action_queue.enqueue(RemoveTimerAction{ t_ });

    // TODO don't blindly return 0 here; requires some response from the timer thread to know what the returned value was
    return 0;
}

#ifdef _WIN32

// The implementations of std::chrono::sleep_until and sleep_for were affected by changing the system clock backwards in older versions
// of Microsoft's STL. This was fixed as of Visual Studio 2022 17.9, but to be safe ultramodern uses Win32 Sleep directly.
void ultramodern::sleep_milliseconds(uint32_t millis) {
    Sleep(millis);
}

void ultramodern::sleep_until(const std::chrono::high_resolution_clock::time_point& time_point) {
    auto time_now = std::chrono::high_resolution_clock::now();
    if (time_point > time_now) {
        long long delta_ms = std::chrono::ceil<std::chrono::milliseconds>(time_point - time_now).count();
        // printf("Sleeping %lld %d ms\n", delta_ms, (uint32_t)delta_ms);
        Sleep(delta_ms);
    }
}

#else

void ultramodern::sleep_milliseconds(uint32_t millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds{millis});
}

void ultramodern::sleep_until(const std::chrono::high_resolution_clock::time_point& time_point) {
    std::this_thread::sleep_until(time_point);
}

#endif
