// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include "ApiGlobal.h"

namespace Api
{

/**
 * Runs work on FreeCAD's main thread. Transports receive on their own threads and post the
 * dispatch here, because FreeCAD's document model is not thread safe.
 */
class ApiExport Executor
{
public:
    virtual ~Executor() = default;
    virtual void post(std::function<void()> task) = 0;

    /**
     * The executor for this process: Qt's event loop when a QCoreApplication exists (the
     * desktop FreeCAD), otherwise the loop run by runMainLoop().
     */
    static Executor& main();

    /// Run posted tasks on the calling thread until stopMainLoop(). Only without Qt.
    static void runMainLoop();
    static void stopMainLoop();
};

/// A queue drained by the thread that calls run().
class ApiExport QueueExecutor: public Executor
{
public:
    void post(std::function<void()> task) override;
    void run();
    void stop();

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<std::function<void()>> m_tasks;
    bool m_stopped = false;
};

}  // namespace Api
