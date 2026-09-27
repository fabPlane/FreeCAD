// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <exception>
#include <string>

#include <App/Application.h>
#include <App/Document.h>

namespace Api
{

/**
 * Makes an editing command one undo step, the way a GUI command is, unless the client already
 * opened a transaction (OpenTransaction), in which case the change joins it. The step is
 * committed when the command succeeds and aborted when it throws.
 */
class AutoTransaction
{
public:
    AutoTransaction(App::Document* doc, const std::string& name)
        : m_doc(doc)
        , m_owns(!inTransaction(doc))
        , m_exceptions(std::uncaught_exceptions())
    {
        if (m_owns) {
            m_doc->openTransaction(name);
        }
    }

    ~AutoTransaction()
    {
        if (!m_owns) {
            return;
        }
        try {
            if (std::uncaught_exceptions() > m_exceptions) {
                m_doc->abortTransaction();
            }
            else {
                m_doc->commitTransaction();
            }
        }
        catch (...) {
        }
    }

    /// A transaction is open, or booked by OpenTransaction but not started by a change yet.
    static bool inTransaction(const App::Document* doc)
    {
        return doc->hasPendingTransaction() || doc->getBookedTransactionID() != 0
            || App::GetApplication().getGlobalTransaction() != 0;
    }

    AutoTransaction(const AutoTransaction&) = delete;
    AutoTransaction& operator=(const AutoTransaction&) = delete;

private:
    App::Document* m_doc;
    bool m_owns;
    int m_exceptions;
};

}  // namespace Api
