/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "session_command.hh"
#include <maxscale/modutil.h>
#include <maxscale/protocol/mysql.h>

void SessionCommand::mark_reply_received()
{
    m_replySent = true;
}

bool SessionCommand::is_reply_received() const
{
    return m_replySent;
}

uint8_t SessionCommand::get_command() const
{
    return m_command;
}

uint64_t SessionCommand::get_position() const
{
    return m_pos;
}

Buffer SessionCommand::copy_buffer() const
{
    return m_buffer;
}

SessionCommand::SessionCommand(GWBUF *buffer, uint64_t id):
    m_buffer(buffer),
    m_command(0),
    m_pos(id),
    m_replySent(false)
{
    if (buffer)
    {
        gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &m_command);
    }
}

SessionCommand::~SessionCommand()
{
}

std::string SessionCommand::to_string()
{
    std::string str;
    char *sql;
    int sql_len;

    /** TODO: Create C++ versions of modutil functions  */
    GWBUF *buf = m_buffer.release();

    if (modutil_extract_SQL(buf, &sql, &sql_len))
    {
        str.append(sql, sql_len);
    }

    m_buffer.reset(buf);

    return str;
}
