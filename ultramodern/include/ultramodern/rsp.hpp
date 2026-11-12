#ifndef __RSP_HPP__
#define __RSP_HPP__

#include <cstdint>

#include "ultra64.h"

namespace ultramodern {
    namespace rsp {
        struct callbacks_t {
            using init_t = void();
            using run_microcode_t = bool(RDRAM_ARG const OSTask* task);

            init_t* init;

            /**
             * Executes the given RSP task.
             *
             * Returns true if task was executed successfully.
             */
            run_microcode_t* run_task;
        };

        void set_callbacks(const callbacks_t& callbacks);

        // Where ULTRAMODERN_RSP_TASKS_COPIED is on, copies an RSP task other than graphics, its OSTask and data, as the
        // game starts it (librecomp's osSpTaskStartGo), and run_task runs the task from the copy (rsp.cpp says why).
        void task_submitted(RDRAM_ARG PTR(OSTask) task);

        // The game asked the RSP's task to yield (librecomp's osSpTaskYield). Where
        // ULTRAMODERN_YIELDS_END_QUEUED_GRAPHICS is on, a graphics task the renderer hasn't started gets its RSP done
        // now (rsp.cpp says why); otherwise nothing.
        void yield_asked();

        // Whether the graphics task the renderer takes still needs its RSP done sent, and if so takes it as sent: not
        // where yield_asked sent it already. Always true where ULTRAMODERN_YIELDS_END_QUEUED_GRAPHICS is off.
        bool graphics_sp_due();

        // The data of the task the RSP is running (its OSTask's data_ptr and data_size) as the game submitted it, where
        // the task runs from a copy (task_submitted): an RSP DMA from RDRAM reads the copy wherever it covers and a DMA
        // to RDRAM writes it too (librecomp's rsp.hpp). Its size is 0 otherwise.
        struct SubmittedData {
            uint32_t address; // physical, a multiple of 8
            uint32_t size; // a multiple of 8
            uint8_t* bytes; // laid out as RDRAM holds them
        };
        extern SubmittedData submitted_data;

        void init();
        bool run_task(RDRAM_ARG const OSTask* task);
    };
} // namespace ultramodern

#endif
