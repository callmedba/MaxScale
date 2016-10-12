#ifndef TEST_UTILS_H
#define TEST_UTILS_H

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

#include <maxscale/poll.h>
#include <dcb.h>
#include <housekeeper.h>
#include <maxscale_test.h>
#include <log_manager.h>
#include <statistics.h>

void init_test_env(char *path)
{
    int argc = 3;

    const char* logdir = path ? path : TEST_LOG_DIR;

    ts_stats_init();
    mxs_log_init(NULL, logdir, MXS_LOG_TARGET_DEFAULT);
    poll_init();
    hkinit();
}

#endif
