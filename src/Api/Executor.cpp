// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Executor.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <Base/Console.h>

namespace Api
{

namespace
{

class QtExecutor: public Executor
{
public:
    void post(std::function<void()> task) override
    {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [task = std::move(task)]() { task(); },
            Qt::QueuedConnection
        );
    }
};

QueueExecutor& queueExecutor()
{
    static QueueExecutor executor;
    return executor;
}

void runTask(const std::function<void()>& task)
{
    try {
        task();
    }
    catch (const std::exception& e) {
        Base::Console().error("FreeCADApi: {}\n", e.what());
    }
    catch (...) {
        Base::Console().error("FreeCADApi: unknown error in a posted task\n");
    }
}

}  // namespace

Executor& Executor::main()
{
    static QtExecutor qt;
    if (QCoreApplication::instance()) {
        return qt;
    }
    return queueExecutor();
}

void Executor::runMainLoop()
{
    queueExecutor().run();
}

void Executor::stopMainLoop()
{
    queueExecutor().stop();
}

void QueueExecutor::post(std::function<void()> task)
{
    {
        std::lock_guard lock(m_mutex);
        m_tasks.push_back(std::move(task));
    }
    m_ready.notify_one();
}

void QueueExecutor::run()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopped = false;
    }
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_ready.wait(lock, [this] { return m_stopped || !m_tasks.empty(); });
            if (m_tasks.empty()) {
                return;  // stopped and drained
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        runTask(task);
    }
}

void QueueExecutor::stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopped = true;
    }
    m_ready.notify_all();
}

}  // namespace Api
