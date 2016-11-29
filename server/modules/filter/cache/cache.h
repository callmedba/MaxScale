#pragma once
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/cdefs.h>
#include <maxscale/buffer.h>
#include <maxscale/filter.h>
#include <maxscale/session.h>
#include "cachefilter.h"
#include "cache_storage_api.h"

class SessionCache;

class Cache
{
public:
    ~Cache();

    /**
     * Returns whether the results of a particular query should be stored.
     *
     * @param zDefaultDb  The current default database.
     * @param pQuery      Buffer containing a SELECT.
     *
     * @return True of the result should be cached.
     */
    bool shouldStore(const char* zDefaultDb, const GWBUF* pQuery);

    /**
     * Returns whether cached results should be used.
     *
     * @param pSession  The session in question.
     *
     * @return True of cached results should be used.
     */
    bool shouldUse(const SESSION* pSession);

    /**
     * Specifies whether a particular SessioCache should refresh the data.
     *
     * @param pKey           The hashed key for a query.
     * @param pSessionCache  The session cache asking.
     *
     * @return True, if the session cache should refresh the data.
     */
    virtual bool mustRefresh(const char* pKey, const SessionCache* pSessionCache);

    /**
     * To inform the cache that a particular item has been updated upon request.
     *
     * @param pKey           The hashed key for a query.
     * @param pSessionCache  The session cache informing.
     */
    virtual void refreshed(const char* pKey,  const SessionCache* pSessionCache);

    const CACHE_CONFIG& config() const { return m_config; }

    cache_result_t getKey(const char* zDefaultDb, const GWBUF* pQuery, char* pKey);

    cache_result_t getValue(const char* pKey, uint32_t flags, GWBUF** ppValue);

    cache_result_t putValue(const char* pKey, const GWBUF* pValue);

    cache_result_t delValue(const char* pKey);

protected:
    Cache(const char* zName,
          CACHE_CONFIG& config,
          CACHE_RULES* pRules,
          StorageFactory* pFactory,
          Storage* pStorage,
          HASHTABLE* pPending);

    static bool Create(const CACHE_CONFIG& config,
                       CACHE_RULES**       ppRules,
                       StorageFactory**    ppFactory,
                       HASHTABLE**         ppPending);

    long hashOfKey(const char* pKey);

    bool mustRefresh(long key, const SessionCache* pSessionCache);

    void refreshed(long key, const SessionCache* pSessionCache);

private:
    Cache(const Cache&);
    Cache& operator = (const Cache&);

protected:
    const char*     m_zName;       // The name of the instance; the section name in the config.
    CACHE_CONFIG    m_config;      // The configuration of the cache instance.
    CACHE_RULES*    m_pRules;      // The rules of the cache instance.
    StorageFactory* m_pFactory;    // The storage factory.
    Storage*        m_pStorage;    // The storage instance to use.
    HASHTABLE*      m_pPending;    // Pending items; being fetched from the backend.
    SPINLOCK        m_lockPending; // Lock used for protecting 'pending'.
};