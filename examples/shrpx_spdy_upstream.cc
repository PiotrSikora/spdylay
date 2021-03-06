/*
 * Spdylay - SPDY Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_spdy_upstream.h"

#include <assert.h>
#include <sstream>

#include "shrpx_client_handler.h"
#include "shrpx_downstream.h"
#include "shrpx_downstream_connection.h"
#include "shrpx_config.h"
#include "shrpx_http.h"
#include "util.h"

using namespace spdylay;

namespace shrpx {

namespace {
const size_t SHRPX_SPDY_UPSTREAM_OUTPUT_UPPER_THRES = 64*1024;
} // namespace

namespace {
ssize_t send_callback(spdylay_session *session,
                      const uint8_t *data, size_t len, int flags,
                      void *user_data)
{
  int rv;
  SpdyUpstream *upstream = reinterpret_cast<SpdyUpstream*>(user_data);
  ClientHandler *handler = upstream->get_client_handler();
  bufferevent *bev = handler->get_bev();
  evbuffer *output = bufferevent_get_output(bev);
  // Check buffer length and return WOULDBLOCK if it is large enough.
  if(evbuffer_get_length(output) > SHRPX_SPDY_UPSTREAM_OUTPUT_UPPER_THRES) {
    return SPDYLAY_ERR_WOULDBLOCK;
  }

  rv = evbuffer_add(output, data, len);
  if(rv == -1) {
    return SPDYLAY_ERR_CALLBACK_FAILURE;
  } else {
    return len;
  }
}
} // namespace

namespace {
ssize_t recv_callback(spdylay_session *session,
                      uint8_t *data, size_t len, int flags, void *user_data)
{
  SpdyUpstream *upstream = reinterpret_cast<SpdyUpstream*>(user_data);
  ClientHandler *handler = upstream->get_client_handler();
  bufferevent *bev = handler->get_bev();
  evbuffer *input = bufferevent_get_input(bev);
  int nread = evbuffer_remove(input, data, len);
  if(nread == -1) {
    return SPDYLAY_ERR_CALLBACK_FAILURE;
  } else if(nread == 0) {
    return SPDYLAY_ERR_WOULDBLOCK;
  } else {
    return nread;
  }
}
} // namespace

namespace {
void on_stream_close_callback
(spdylay_session *session, int32_t stream_id, spdylay_status_code status_code,
 void *user_data)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "Upstream spdy Stream " << stream_id << " is being closed";
  }
  SpdyUpstream *upstream = reinterpret_cast<SpdyUpstream*>(user_data);
  Downstream *downstream = upstream->find_downstream(stream_id);
  if(downstream) {
    if(downstream->get_request_state() == Downstream::CONNECT_FAIL) {
      upstream->remove_downstream(downstream);
      delete downstream;
    } else {
      downstream->set_request_state(Downstream::STREAM_CLOSED);
      if(downstream->get_response_state() == Downstream::MSG_COMPLETE) {
        // At this point, downstream response was read
        if(!downstream->get_response_connection_close()) {
          // Keep-alive
          DownstreamConnection *dconn;
          dconn = downstream->get_downstream_connection();
          if(dconn) {
            dconn->detach_downstream(downstream);
          }
        }
        upstream->remove_downstream(downstream);
        delete downstream;
      } else {
        // At this point, downstream read may be paused.
        upstream->remove_downstream(downstream);
        delete downstream;
        // How to test this case? Request sufficient large download
        // and make client send RST_STREAM after it gets first DATA
        // frame chunk.
      }
    }
  }
}
} // namespace

namespace {
void on_ctrl_recv_callback
(spdylay_session *session, spdylay_frame_type type, spdylay_frame *frame,
 void *user_data)
{
  SpdyUpstream *upstream = reinterpret_cast<SpdyUpstream*>(user_data);
  switch(type) {
  case SPDYLAY_SYN_STREAM: {
    if(ENABLE_LOG) {
      LOG(INFO) << "Upstream spdy received upstream SYN_STREAM stream_id="
                << frame->syn_stream.stream_id;
    }
    Downstream *downstream;
    downstream = new Downstream(upstream,
                                frame->syn_stream.stream_id,
                                frame->syn_stream.pri);
    upstream->add_downstream(downstream);
    downstream->init_response_body_buf();

    char **nv = frame->syn_stream.nv;
    for(size_t i = 0; nv[i]; i += 2) {
      if(strcmp(nv[i], ":path") == 0) {
        downstream->set_request_path(nv[i+1]);
      } else if(strcmp(nv[i], ":method") == 0) {
        downstream->set_request_method(nv[i+1]);
      } else if(nv[i][0] != ':') {
        downstream->add_request_header(nv[i], nv[i+1]);
      }
    }
    downstream->add_request_header("X-Forwarded-Spdy", "true");

    if(ENABLE_LOG) {
      std::stringstream ss;
      for(size_t i = 0; nv[i]; i += 2) {
        ss << nv[i] << ": " << nv[i+1] << "\n";
      }
      LOG(INFO) << "Upstream spdy request headers:\n" << ss.str();
    }

    DownstreamConnection *dconn;
    dconn = upstream->get_client_handler()->get_downstream_connection();
    int rv = dconn->attach_downstream(downstream);
    if(rv != 0) {
      // If downstream connection fails, issue RST_STREAM.
      upstream->rst_stream(downstream, SPDYLAY_INTERNAL_ERROR);
      downstream->set_request_state(Downstream::CONNECT_FAIL);
      return;
    }
    downstream->push_request_headers();
    downstream->set_request_state(Downstream::HEADER_COMPLETE);
    if(frame->syn_stream.hd.flags & SPDYLAY_CTRL_FLAG_FIN) {
      if(ENABLE_LOG) {
        LOG(INFO) << "Upstream spdy "
                  << "Setting Downstream::MSG_COMPLETE for Downstream "
                  << downstream;
      }
      downstream->set_request_state(Downstream::MSG_COMPLETE);
    }
    break;
  }
  default:
    break;
  }
}
} // namespace

namespace {
void on_data_chunk_recv_callback(spdylay_session *session,
                                 uint8_t flags, int32_t stream_id,
                                 const uint8_t *data, size_t len,
                                 void *user_data)
{
  SpdyUpstream *upstream = reinterpret_cast<SpdyUpstream*>(user_data);
  Downstream *downstream = upstream->find_downstream(stream_id);
  if(downstream) {
    downstream->push_upload_data_chunk(data, len);
    if(upstream->get_flow_control()) {
      downstream->inc_recv_window_size(len);
      if(downstream->get_recv_window_size() >
         upstream->get_initial_window_size()) {
        if(ENABLE_LOG) {
          LOG(INFO) << "Flow control error: recv_window_size="
                    << downstream->get_recv_window_size()
                    << ", initial_window_size="
                    << upstream->get_initial_window_size();
        }
        upstream->rst_stream(downstream, SPDYLAY_FLOW_CONTROL_ERROR);
        return;
      }
    }
    if(flags & SPDYLAY_DATA_FLAG_FIN) {
      if(ENABLE_LOG) {
        LOG(INFO) << "Upstream spdy "
                  << "setting Downstream::MSG_COMPLETE for Downstream "
                  << downstream;
      }
      downstream->set_request_state(Downstream::MSG_COMPLETE);
    }
  }
}
} // namespace

SpdyUpstream::SpdyUpstream(uint16_t version, ClientHandler *handler)
  : handler_(handler),
    session_(0)
{
  //handler->set_bev_cb(spdy_readcb, 0, spdy_eventcb);
  handler->set_upstream_timeouts(&get_config()->spdy_upstream_read_timeout,
                                 &get_config()->spdy_upstream_write_timeout);

  spdylay_session_callbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.send_callback = send_callback;
  callbacks.recv_callback = recv_callback;
  callbacks.on_stream_close_callback = on_stream_close_callback;
  callbacks.on_ctrl_recv_callback = on_ctrl_recv_callback;
  callbacks.on_data_chunk_recv_callback = on_data_chunk_recv_callback;

  int rv;
  rv = spdylay_session_server_new(&session_, version, &callbacks, this);
  assert(rv == 0);

  if(version == SPDYLAY_PROTO_SPDY3) {
    int val = 1;
    flow_control_ = true;
    initial_window_size_ = 64*1024; // specified by SPDY/3 spec.
    rv = spdylay_session_set_option(session_,
                                    SPDYLAY_OPT_NO_AUTO_WINDOW_UPDATE, &val,
                                    sizeof(val));
    assert(rv == 0);
  } else {
    flow_control_ = false;
    initial_window_size_ = 0;
  }
  // TODO Maybe call from outside?
  spdylay_settings_entry entry[2];
  entry[0].settings_id = SPDYLAY_SETTINGS_MAX_CONCURRENT_STREAMS;
  entry[0].value = get_config()->spdy_max_concurrent_streams;
  entry[0].flags = SPDYLAY_ID_FLAG_SETTINGS_NONE;

  entry[1].settings_id = SPDYLAY_SETTINGS_INITIAL_WINDOW_SIZE;
  entry[1].value = initial_window_size_;
  entry[1].flags = SPDYLAY_ID_FLAG_SETTINGS_NONE;

  rv = spdylay_submit_settings
    (session_, SPDYLAY_FLAG_SETTINGS_NONE,
     entry, sizeof(entry)/sizeof(spdylay_settings_entry));
  assert(rv == 0);
  // TODO Maybe call from outside?
  send();
}

SpdyUpstream::~SpdyUpstream()
{
  spdylay_session_del(session_);
}

int SpdyUpstream::on_read()
{
  int rv;
  if((rv = spdylay_session_recv(session_)) ||
     (rv = spdylay_session_send(session_))) {
    if(rv != SPDYLAY_ERR_EOF) {
      LOG(ERROR) << "spdylay error: " << spdylay_strerror(rv);
      DIE();
    }
  }
  return 0;
}

int SpdyUpstream::on_write()
{
  send();
  return 0;
}

// After this function call, downstream may be deleted.
int SpdyUpstream::send()
{
  int rv;
  if((rv = spdylay_session_send(session_))) {
    LOG(ERROR) << "spdylay error: " << spdylay_strerror(rv);
    DIE();
  }
  return 0;
}

int SpdyUpstream::on_event()
{
  return 0;
}

ClientHandler* SpdyUpstream::get_client_handler() const
{
  return handler_;
}

namespace {
void spdy_downstream_readcb(bufferevent *bev, void *ptr)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "spdy_downstream_readcb";
  }
  DownstreamConnection *dconn = reinterpret_cast<DownstreamConnection*>(ptr);
  Downstream *downstream = dconn->get_downstream();
  SpdyUpstream *upstream;
  upstream = static_cast<SpdyUpstream*>(downstream->get_upstream());
  if(downstream->get_request_state() == Downstream::STREAM_CLOSED) {
    // If upstream SPDY stream was closed, we just close downstream,
    // because there is no consumer now. Downstream connection is also
    // closed in this case.
    upstream->remove_downstream(downstream);
    delete downstream;
    return;
  }
  int rv = downstream->parse_http_response();
  if(rv != 0) {
    if(ENABLE_LOG) {
      LOG(INFO) << "Downstream HTTP parser failure";
    }
    if(downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
      upstream->rst_stream(downstream, SPDYLAY_INTERNAL_ERROR);
    } else {
      upstream->error_reply(downstream, 502);
    }
    downstream->set_response_state(Downstream::MSG_COMPLETE);
    // Clearly, we have to close downstream connection on http parser
    // failure.
    downstream->set_downstream_connection(0);
    delete dconn;
    dconn = 0;
  }
  upstream->send();
  // At this point, downstream may be deleted.
}
} // namespace

namespace {
void spdy_downstream_writecb(bufferevent *bev, void *ptr)
{
  DownstreamConnection *dconn = reinterpret_cast<DownstreamConnection*>(ptr);
  Downstream *downstream = dconn->get_downstream();
  SpdyUpstream *upstream;
  upstream = static_cast<SpdyUpstream*>(downstream->get_upstream());
  if(upstream->get_flow_control()) {
    if(downstream->get_recv_window_size() >=
       upstream->get_initial_window_size()/2) {
      upstream->window_update(downstream);
    }
  }
}
} // namespace

namespace {
void spdy_downstream_eventcb(bufferevent *bev, short events, void *ptr)
{
  DownstreamConnection *dconn = reinterpret_cast<DownstreamConnection*>(ptr);
  Downstream *downstream = dconn->get_downstream();
  SpdyUpstream *upstream;
  upstream = static_cast<SpdyUpstream*>(downstream->get_upstream());
  if(events & BEV_EVENT_CONNECTED) {
    if(ENABLE_LOG) {
      LOG(INFO) << "Downstream connection established. Downstream "
                << downstream;
    }
  } else if(events & BEV_EVENT_EOF) {
    if(ENABLE_LOG) {
      LOG(INFO) << "Downstream EOF stream_id="
                << downstream->get_stream_id();
    }
    if(downstream->get_request_state() == Downstream::STREAM_CLOSED) {
      // If stream was closed already, we don't need to send reply at
      // the first place. We can delete downstream.
      upstream->remove_downstream(downstream);
      delete downstream;
    } else {
      // Delete downstream connection. If we don't delete it here, it
      // will be pooled in on_stream_close_callback.
      downstream->set_downstream_connection(0);
      delete dconn;
      dconn = 0;
      // downstream wil be deleted in on_stream_close_callback.
      if(downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
        // Server may indicate the end of the request by EOF
        if(ENABLE_LOG) {
          LOG(INFO) << "Assuming downstream content-length is 0 byte";
        }
        downstream->set_response_state(Downstream::MSG_COMPLETE);
        upstream->on_downstream_body_complete(downstream);
      } else if(downstream->get_response_state() != Downstream::MSG_COMPLETE) {
        // If stream was not closed, then we set MSG_COMPLETE and let
        // on_stream_close_callback delete downstream.
        upstream->error_reply(downstream, 502);
        downstream->set_response_state(Downstream::MSG_COMPLETE);
        upstream->send();
        // At this point, downstream may be deleted.
      }
    }
  } else if(events & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
    if(ENABLE_LOG) {
      LOG(INFO) << "Downstream error/timeout. Downstream " << downstream;
    }
    if(downstream->get_request_state() == Downstream::STREAM_CLOSED) {
      upstream->remove_downstream(downstream);
      delete downstream;
    } else {
      // Delete downstream connection. If we don't delete it here, it
      // will be pooled in on_stream_close_callback.
      downstream->set_downstream_connection(0);
      delete dconn;
      dconn = 0;
      if(downstream->get_response_state() != Downstream::MSG_COMPLETE) {
        if(downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
          upstream->rst_stream(downstream, SPDYLAY_INTERNAL_ERROR);
        } else {
          int status;
          if(events & BEV_EVENT_TIMEOUT) {
            status = 504;
          } else {
            status = 502;
          }
          upstream->error_reply(downstream, status);
        }
        downstream->set_response_state(Downstream::MSG_COMPLETE);
        upstream->send();
        // At this point, downstream may be deleted.
      }
    }
  }
}
} // namespace

int SpdyUpstream::rst_stream(Downstream *downstream, int status_code)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "RST_STREAM stream_id="
              << downstream->get_stream_id();
  }
  int rv;
  rv = spdylay_submit_rst_stream(session_, downstream->get_stream_id(),
                                 status_code);
  if(rv < SPDYLAY_ERR_FATAL) {
    DIE();
  } else {
    return 0;
  }
}

int SpdyUpstream::window_update(Downstream *downstream)
{
  int rv;
  rv = spdylay_submit_window_update(session_, downstream->get_stream_id(),
                                    downstream->get_recv_window_size());
  downstream->set_recv_window_size(0);
  if(rv < SPDYLAY_ERR_FATAL) {
    DIE();
  } else {
    return 0;
  }
}

namespace {
ssize_t spdy_data_read_callback(spdylay_session *session,
                                int32_t stream_id,
                                uint8_t *buf, size_t length,
                                int *eof,
                                spdylay_data_source *source,
                                void *user_data)
{
  Downstream *downstream = reinterpret_cast<Downstream*>(source->ptr);
  evbuffer *body = downstream->get_response_body_buf();
  assert(body);
  int nread = evbuffer_remove(body, buf, length);
  if(nread == 0 &&
     downstream->get_response_state() == Downstream::MSG_COMPLETE) {
    *eof = 1;
  }
  if(nread == 0 && *eof != 1) {
    return SPDYLAY_ERR_DEFERRED;
  }
  return nread;
}
} // namespace

int SpdyUpstream::error_reply(Downstream *downstream, int status_code)
{
  int rv;
  std::string html = http::create_error_html(status_code);
  downstream->init_response_body_buf();
  evbuffer *body = downstream->get_response_body_buf();
  rv = evbuffer_add(body, html.c_str(), html.size());
  if(rv == -1) {
    DIE();
  }
  downstream->set_response_state(Downstream::MSG_COMPLETE);
  
  spdylay_data_provider data_prd;
  data_prd.source.ptr = downstream;
  data_prd.read_callback = spdy_data_read_callback;

  const char *nv[] = {
    ":status", http::get_status_string(status_code),
    ":version", "http/1.1",
    "content-type", "text/html; charset=UTF-8",
    "server", get_config()->server_name,
    0
  };

  rv = spdylay_submit_response(session_, downstream->get_stream_id(), nv,
                               &data_prd);
  if(rv < SPDYLAY_ERR_FATAL) {
    DIE();
  } else {
    return 0;
  }
}

bufferevent_data_cb SpdyUpstream::get_downstream_readcb()
{
  return spdy_downstream_readcb;
}

bufferevent_data_cb SpdyUpstream::get_downstream_writecb()
{
  return spdy_downstream_writecb;
}

bufferevent_event_cb SpdyUpstream::get_downstream_eventcb()
{
  return spdy_downstream_eventcb;
}

void SpdyUpstream::add_downstream(Downstream *downstream)
{
  downstream_queue_.add(downstream);
}

void SpdyUpstream::remove_downstream(Downstream *downstream)
{
  downstream_queue_.remove(downstream);
}

Downstream* SpdyUpstream::find_downstream(int32_t stream_id)
{
  return downstream_queue_.find(stream_id);
}

spdylay_session* SpdyUpstream::get_spdy_session()
{
  return session_;
}

// WARNING: Never call directly or indirectly spdylay_session_send or
// spdylay_session_recv. These calls may delete downstream.
int SpdyUpstream::on_downstream_header_complete(Downstream *downstream)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "Downstream on_downstream_header_complete";
  }
  size_t nheader = downstream->get_response_headers().size();
  // 6 means :status, :version and possible via header field.
  const char **nv = new const char*[nheader * 2 + 6 + 1];
  size_t hdidx = 0;
  std::string via_value;
  std::string location;
  nv[hdidx++] = ":status";
  nv[hdidx++] = http::get_status_string(downstream->get_response_http_status());
  nv[hdidx++] = ":version";
  nv[hdidx++] = "HTTP/1.1";
  for(Headers::const_iterator i = downstream->get_response_headers().begin();
      i != downstream->get_response_headers().end(); ++i) {
    if(util::strieq((*i).first.c_str(), "transfer-encoding") ||
       util::strieq((*i).first.c_str(), "keep-alive") || // HTTP/1.0?
       util::strieq((*i).first.c_str(), "connection") ||
       util:: strieq((*i).first.c_str(), "proxy-connection")) {
      // These are ignored
    } else if(util::strieq((*i).first.c_str(), "via")) {
      via_value = (*i).second;
    } else if(util::strieq((*i).first.c_str(), "location")) {
      location = (*i).second;
    } else {
      nv[hdidx++] = (*i).first.c_str();
      nv[hdidx++] = (*i).second.c_str();
    }
  }
  if(!location.empty()) {
    nv[hdidx++] = "location";
    // Assign location to store the result. Otherwise we lose the
    // return value.
    location = http::modify_location_header_value(location);
    nv[hdidx++] = location.c_str();
  }
  if(!via_value.empty()) {
    via_value += ", ";
  }
  via_value += http::create_via_header_value(downstream->get_response_major(),
                                             downstream->get_response_minor());
  nv[hdidx++] = "via";
  nv[hdidx++] = via_value.c_str();
  nv[hdidx++] = 0;
  if(ENABLE_LOG) {
    std::stringstream ss;
    for(size_t i = 0; nv[i]; i += 2) {
      ss << nv[i] << ": " << nv[i+1] << "\n";
    }
    LOG(INFO) << "Upstream spdy response headers\n" << ss.str();
  }
  spdylay_data_provider data_prd;
  data_prd.source.ptr = downstream;
  data_prd.read_callback = spdy_data_read_callback;

  spdylay_submit_response(session_, downstream->get_stream_id(), nv,
                          &data_prd);
  delete [] nv;
  return 0;
}

// WARNING: Never call directly or indirectly spdylay_session_send or
// spdylay_session_recv. These calls may delete downstream.
int SpdyUpstream::on_downstream_body(Downstream *downstream,
                                     const uint8_t *data, size_t len)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "Downstream on_downstream_body";
  }
  evbuffer *body = downstream->get_response_body_buf();
  evbuffer_add(body, data, len);
  spdylay_session_resume_data(session_, downstream->get_stream_id());

  size_t bodylen = evbuffer_get_length(body);
  if(bodylen > SHRPX_SPDY_UPSTREAM_OUTPUT_UPPER_THRES) {
    downstream->pause_read(SHRPX_NO_BUFFER);
  }

  return 0;
}

// WARNING: Never call directly or indirectly spdylay_session_send or
// spdylay_session_recv. These calls may delete downstream.
int SpdyUpstream::on_downstream_body_complete(Downstream *downstream)
{
  if(ENABLE_LOG) {
    LOG(INFO) << "Downstream on_downstream_body_complete";
  }
  spdylay_session_resume_data(session_, downstream->get_stream_id());
  return 0;
}

bool SpdyUpstream::get_flow_control() const
{
  return flow_control_;
}

int32_t SpdyUpstream::get_initial_window_size() const
{
  return initial_window_size_;
}

} // namespace shrpx
