#include <cassert>
#include <cstring>

#include "ultramodern/rsp.hpp"

#if ULTRAMODERN_YIELDS_END_QUEUED_GRAPHICS
#include <atomic>
#endif

#if ULTRAMODERN_RSP_TASKS_COPIED
#include <iterator>
#include <memory>
#include <mutex>
#include <vector>
#endif

static ultramodern::rsp::callbacks_t rsp_callbacks {};

ultramodern::rsp::SubmittedData ultramodern::rsp::submitted_data {};

void ultramodern::rsp::set_callbacks(const callbacks_t& callbacks) {
    rsp_callbacks = callbacks;
}

void ultramodern::rsp::init() {
    if (rsp_callbacks.init != nullptr) {
        rsp_callbacks.init();
    }
}

#if ULTRAMODERN_RSP_TASKS_COPIED
// An RSP task as the game submitted it: its OSTask and the RDRAM its data_ptr and data_size give (an audio task's
// command list), copied as the game starts it (task_submitted). A task the page runs late would read them after the game
// had written over them: a game expects its audio task done within the VI it starts in, as the RSP does it, and Super
// Mario 64's sound thread writes the next list but one into the same buffer and OSTask two VIs after the last, whether
// the task has run or not. Started late and still reading its list in chunks, the task read the start of one list and
// the rest of the next, whose commands wrote over aspMain's jump table in DMEM: it exited at an unhandled jump target.
// With the copy, a task runs on a whole list however late it runs.
struct SubmittedTask {
    const OSTask* original;
    OSTask task;
    uint32_t data_address;
    std::vector<uint8_t> data;
};

// The newest copy for each OSTask the game started a task with, the eight it used last. A task run late takes the
// newest copy of its OSTask, as it would have read the newest list from RDRAM: one that never runs (a runtime may
// complete a task unrun) leaves nothing behind, and a task whose copy is gone runs from RDRAM as before.
static struct {
    std::mutex mutex;
    std::shared_ptr<const SubmittedTask> tasks[8];
    size_t next = 0;
} submitted_tasks;
#endif

#if ULTRAMODERN_YIELDS_END_QUEUED_GRAPHICS
// The graphics thread tells the game its RSP task is done as it takes the task (events.cpp, gfx_thread_func), and ignores
// a yield, so a task queued behind the renderer's other actions answers no yield until the renderer gets to it. A
// console's RSP answers one within a few ms, and a game's scheduler may take a yield left unanswered at its next VI for a
// hung RSP: Beetle Adventure Racing's asks one of the graphics task whenever an audio task comes, and at the next VI
// sends itself the RSP's and RDP's done. Those end the queued task before the renderer has read it, and the real ones
// then end the next, so the game took lists for done and reused what they point at: the renderer ran from a list into
// what the game had put there since, loading vertices until the page ran out of memory, or the scheduler's thread queues
// broke. On a page with render to RAM's late copy, which tells the game a list is done before the renderer submits it,
// the next list waits queued that long often enough that the game never got from its menus into a race.
// So a yield asked while a graphics task waits sends that task's RSP done at once, as if the RSP had finished before
// the request (what osSpTaskYielded already answers), and the graphics thread sends none as it takes the task. The
// RDP's done still comes once the renderer has read the list, and a game waits for both before it reuses a list.

// events.cpp's: sends the game the RSP's done.
void sp_complete();

// Graphics tasks started whose RSP done hasn't gone yet.
static std::atomic<uint32_t> graphics_sp_owed{ 0 };

bool ultramodern::rsp::graphics_sp_due() {
    uint32_t owed = graphics_sp_owed.load();
    while (owed > 0) {
        if (graphics_sp_owed.compare_exchange_weak(owed, owed - 1)) {
            return true;
        }
    }

    return false;
}

void ultramodern::rsp::yield_asked() {
    if (graphics_sp_due()) {
        sp_complete();
    }
}
#else
bool ultramodern::rsp::graphics_sp_due() {
    return true;
}

void ultramodern::rsp::yield_asked() {
}
#endif

void ultramodern::rsp::task_submitted(RDRAM_ARG PTR(OSTask) task_) {
#if ULTRAMODERN_YIELDS_END_QUEUED_GRAPHICS
    // Counted before the graphics thread can take it (submit_rsp_task comes after)
    if (TO_PTR(OSTask, task_)->t.type == M_GFXTASK) {
        graphics_sp_owed++;
    }
#endif
#if ULTRAMODERN_RSP_TASKS_COPIED
    const OSTask* task = TO_PTR(OSTask, task_);
    // A graphics task's OSTask goes to the renderer as a copy already (events.cpp, SpTaskAction)
    if (task->t.type == M_GFXTASK) {
        return;
    }
    // Its data in whole 8-byte units, as an RSP DMA reads them
    auto copy = std::make_shared<SubmittedTask>(SubmittedTask{ task, *task, 0, {} });
    const uint32_t data_start = task->t.data_ptr & 0xFFFFF8;
    const uint32_t data_end = ((task->t.data_ptr & 0xFFFFFF) + task->t.data_size + 7) & ~7u;
    if (task->t.data_size != 0 && data_end <= 0x800000) {
        copy->data_address = data_start;
        copy->data.assign(rdram + data_start, rdram + data_end);
    }
    std::lock_guard lock{ submitted_tasks.mutex };
    for (std::shared_ptr<const SubmittedTask>& submitted : submitted_tasks.tasks) {
        if (submitted != nullptr && submitted->original == task) {
            submitted = std::move(copy);
            return;
        }
    }
    submitted_tasks.tasks[submitted_tasks.next] = std::move(copy);
    submitted_tasks.next = (submitted_tasks.next + 1) % std::size(submitted_tasks.tasks);
#endif
}

bool ultramodern::rsp::run_task(RDRAM_ARG const OSTask* task) {
    assert(rsp_callbacks.run_task != nullptr);

#if ULTRAMODERN_RSP_TASKS_COPIED
    std::shared_ptr<const SubmittedTask> copy;
    {
        std::lock_guard lock{ submitted_tasks.mutex };
        for (const std::shared_ptr<const SubmittedTask>& submitted : submitted_tasks.tasks) {
            if (submitted != nullptr && submitted->original == task) {
                copy = submitted;
            }
        }
    }
    if (copy != nullptr) {
        submitted_data = { copy->data_address, static_cast<uint32_t>(copy->data.size()), const_cast<uint8_t*>(copy->data.data()) };
        const bool ran = rsp_callbacks.run_task(PASS_RDRAM &copy->task);
        submitted_data = {};
        return ran;
    }
#endif
    return rsp_callbacks.run_task(PASS_RDRAM task);
}
