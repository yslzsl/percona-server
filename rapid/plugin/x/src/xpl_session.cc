/*
 * Copyright (c) 2015, 2016 Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include "xpl_dispatcher.h"
#include "xpl_session.h"
#include "xpl_log.h"
#include "xpl_server.h"

#include "crud_cmd_handler.h"
#include "sql_data_context.h"
#include "ngs/client.h"
#include "ngs/scheduler.h"
#include "notices.h"
#include "ngs/ngs_error.h"
#include "ngs_common/protocol_protobuf.h"

#include <iostream>


xpl::Session::Session(ngs::Client &client, ngs::Protocol_encoder *proto, Session_id session_id)
: ngs::Session(client, proto, session_id),
  m_sql(new Sql_data_context(proto)),
  m_crud_handler(NULL),
  m_was_authenticated(false)
{
}


xpl::Session::~Session()
{
  if (m_was_authenticated)
    Global_status_variables::instance().decrement_sessions_count();

  m_sql->deinit();

  delete m_sql;
  delete m_crud_handler;
}


// handle a message while in Ready state
bool xpl::Session::handle_ready_message(ngs::Request &command)
{
  DBUG_ASSERT(m_crud_handler != NULL);

  // check if the session got killed
  if (m_sql->is_killed())
  {
    m_encoder->send_result(ngs::Error_code(ER_QUERY_INTERRUPTED, "Query execution was interrupted", "70100", ngs::Error_code::FATAL));
    // close as fatal_error instead of killed. killed is for when the client is idle
    on_close();
    return true;
  }

  if (ngs::Session::handle_ready_message(command))
    return true;

  try
  {
    return dispatcher::dispatch_command(*this, *m_sql, *m_encoder, *m_crud_handler, m_expect_stack,
                                            m_options, command);
  }
  catch (ngs::Error_code &err)
  {
    m_encoder->send_result(err);
    on_close();
    return true;
  }
  catch (std::exception &exc)
  {
    // not supposed to happen, but catch exceptions as a last defense..
    log_error("%s: Unexpected exception dispatching command: %s\n", m_client.client_id(), exc.what());
    on_close();
    return true;
  }
  return false;
}


ngs::Error_code xpl::Session::init()
{
  const unsigned short port = m_client.client_port();
  const bool is_tls_active  = m_client.connection().options()->active_tls();

  return m_sql->init(port, is_tls_active);
}


void xpl::Session::on_kill()
{
  if (m_sql && !m_sql->is_killed())
  {
    if (!m_sql->kill())
      log_info("%s: Could not interrupt client session", m_client.client_id());
  }

  on_close(true);
}


void xpl::Session::on_auth_success(const ngs::Authentication_handler::Response &response)
{
  xpl::notices::send_client_id(proto(), m_client.client_id_num());
  ngs::Session::on_auth_success(response);

  Global_status_variables::instance().increment_accepted_sessions_count();
  Global_status_variables::instance().increment_sessions_count();

  m_was_authenticated = true;
  if (!m_crud_handler)
    m_crud_handler = new Crud_command_handler(*m_sql, m_options, m_status_variables);
}


void xpl::Session::on_auth_failure(const ngs::Authentication_handler::Response &response)
{
  if (response.error_code == ER_MUST_CHANGE_PASSWORD && !m_sql->password_expired())
  {
    ngs::Authentication_handler::Response r = {"Password for " MYSQLXSYS_ACCOUNT " account has been expired", response.status, response.error_code};
    ngs::Session::on_auth_failure(r);
  }
  else
    ngs::Session::on_auth_failure(response);

  Global_status_variables::instance().increment_rejected_sessions_count();
}


/** Checks whether things owned by the given user are visible to this session.
 Returns true if we're SUPER or the same user as the given one.
 If user is NULL, then it's only visible for SUPER users.
 */
bool xpl::Session::can_see_user(const char *user) const
{
  const char *owner;
  if (is_ready() && (owner = m_sql->authenticated_user()))
  {
    if (m_sql->authenticated_user_is_super()
        || (user && owner && strcmp(owner, user) == 0))
      return true;
  }
  return false;
}
