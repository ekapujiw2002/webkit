/*
 * Copyright (C) 2015 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef UniqueIDBDatabaseConnection_h
#define UniqueIDBDatabaseConnection_h

#if ENABLE(INDEXED_DATABASE)

#include "UniqueIDBDatabaseTransaction.h"
#include <wtf/HashMap.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>

namespace WebCore {

class IDBError;
class IDBResultData;

namespace IDBServer {

class IDBConnectionToClient;
class UniqueIDBDatabase;
class UniqueIDBDatabaseTransaction;

class UniqueIDBDatabaseConnection : public RefCounted<UniqueIDBDatabaseConnection> {
public:
    static Ref<UniqueIDBDatabaseConnection> create(UniqueIDBDatabase&, IDBConnectionToClient&);

    ~UniqueIDBDatabaseConnection();

    uint64_t identifier() const { return m_identifier; }
    UniqueIDBDatabase& database() { return m_database; }
    IDBConnectionToClient& connectionToClient() { return m_connectionToClient; }

    void connectionClosedFromClient();

    bool closePending() const { return m_closePending; }

    bool hasNonFinishedTransactions() const;

    void fireVersionChangeEvent(const IDBResourceIdentifier& requestIdentifier, uint64_t requestedVersion);
    UniqueIDBDatabaseTransaction& createVersionChangeTransaction(uint64_t newVersion);

    void establishTransaction(const IDBTransactionInfo&);
    void didAbortTransaction(UniqueIDBDatabaseTransaction&, const IDBError&);
    void didCommitTransaction(UniqueIDBDatabaseTransaction&, const IDBError&);
    void didCreateObjectStore(const IDBResultData&);
    void didDeleteObjectStore(const IDBResultData&);
    void didClearObjectStore(const IDBResultData&);
    void didCreateIndex(const IDBResultData&);
    void didDeleteIndex(const IDBResultData&);
    void didFireVersionChangeEvent(const IDBResourceIdentifier& requestIdentifier);

private:
    UniqueIDBDatabaseConnection(UniqueIDBDatabase&, IDBConnectionToClient&);

    uint64_t m_identifier { 0 };
    UniqueIDBDatabase& m_database;
    IDBConnectionToClient& m_connectionToClient;

    bool m_closePending { false };

    HashMap<IDBResourceIdentifier, RefPtr<UniqueIDBDatabaseTransaction>> m_transactionMap;
};

} // namespace IDBServer
} // namespace WebCore

#endif // ENABLE(INDEXED_DATABASE)
#endif // UniqueIDBDatabaseConnection_h
