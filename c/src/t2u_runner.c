#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <event2/event.h>
#include <event2/util.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#ifdef __GNUC__
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include "t2u.h"
#include "t2u_internal.h"
#include "t2u_thread.h"
#include "t2u_rbtree.h"
#include "t2u_session.h"
#include "t2u_rule.h"
#include "t2u_context.h"
#include "t2u_runner.h"

/* runner proc */
#if defined __GNUC__
    static void* t2u_runner_loop_(void *arg);
#elif defined _MSC_VER
    static DWORD __stdcall t2u_runner_loop_(void * arg);
#else
    #error "Compiler not support."
#endif

static void t2u_runner_control_callback(evutil_socket_t sock, short events, void *arg);

static void t2u_runner_process_udp_callback(evutil_socket_t sock, short events, void *arg);
static void t2u_runner_process_tcp_callback(evutil_socket_t sock, short events, void *arg);

static void t2u_runner_process_accept_callback(evutil_socket_t sock, short events, void *arg);
static void t2u_runner_process_accept_timeout_callback(evutil_socket_t sock, short events, void *arg);

static void t2u_runner_process_connect_timeout_callback(evutil_socket_t sock, short events, void *arg);
static void t2u_runner_process_connect_success_callback(evutil_socket_t sock, short events, void *arg);

static void t2u_runner_process_send_timeout_callback(evutil_socket_t sock, short events, void *arg);

/* some finder */
static t2u_event_data *find_evdata_by_context(rbtree_node *node, t2u_context *context)
{
    if (node)
    {
        t2u_event_data *ret = NULL;
        ret = find_evdata_by_context(node->left, context);
        if (!ret)
        {
            t2u_event_data *ev = (t2u_event_data *)node->data;
            if (ev->context_ == context && ev->rule_ == NULL)
            {
                ret = ev;
            }    
        }
        if (!ret)
        {
            ret = find_evdata_by_context(node->right, context);
        }

        return ret;
    }

    return NULL;
}

static t2u_event_data *find_evdata_by_rule(rbtree_node *node, t2u_rule *rule)
{
    if (node)
    {
        t2u_event_data *ret = NULL;
        ret = find_evdata_by_rule(node->left, rule);
        if (!ret)
        {
            t2u_event_data *ev = (t2u_event_data *)node->data;
            if (ev->rule_ == rule && ev->session_ == NULL)
            {
                ret = ev;
            }    
        }
        if (!ret)
        {
            ret = find_evdata_by_rule(node->right, rule);
        }

        return ret;
    }

    return NULL;
}

static t2u_event_data *find_evdata_by_session(rbtree_node *node, t2u_session *session)
{
    if (node)
    {
        t2u_event_data *ret = NULL;
        ret = find_evdata_by_session(node->left, session);
        if (!ret)
        {
            t2u_event_data *ev = (t2u_event_data *)node->data;
            if (ev->session_ == session)
            {
                ret = ev;
            }    
        }
        if (!ret)
        {
            ret = find_evdata_by_session(node->right, session);
        }

        return ret;
    }

    return NULL;
}



#define MAX_CONTROL_BUFF_LEN (1600)

static void t2u_runner_control_process(t2u_runner *runner, control_data *cdata)
{
    (void) runner;
    assert (NULL != cdata->func_);
    cdata->func_(cdata->arg_);
}

static void t2u_runner_control_callback(evutil_socket_t sock, short events, void *arg)
{

    t2u_runner *runner = (t2u_runner *)arg;
    control_data cdata;
    size_t len = 0;
    struct sockaddr_in addr_c;
    unsigned int addrlen = sizeof(addr_c);

    (void) events;
    assert(t2u_thr_self() == runner->tid_);

    len = recvfrom(sock, (char *)&cdata, sizeof(cdata), 0, (struct sockaddr *) &addr_c, &addrlen);
    if (len <= 0)
    {
        /* todo: error */
    }

    t2u_runner_control_process(runner, &cdata);

    /* send back message. */
    sendto(sock, (char *)&cdata, sizeof(cdata), 0, (const struct sockaddr *)&addr_c, sizeof(addr_c));

}


void t2u_runner_control(t2u_runner *runner, control_data *cdata)
{
    if (t2u_thr_self() == runner->tid_)
    {
        t2u_runner_control_process(runner, cdata);
    }
    else
    {
        int len; 
        t2u_mutex_lock(&runner->mutex_);
        
        send(runner->sock_[1], (char *) cdata, sizeof(control_data), 0);
        len = recv(runner->sock_[1], (char *) cdata, sizeof(control_data), 0);

        if (len > 0)
        {
        }
        else
        {
            /* todo error handle */
        }

        t2u_mutex_unlock(&runner->mutex_);
    }
}

/* process new udp message in */
static void t2u_runner_process_udp_callback(evutil_socket_t sock, short events, void *arg)
{
    int recv_bytes;
    char *buff = (char *) malloc(T2U_MESS_BUFFER_MAX);
    t2u_message *message;
    t2u_event_data *ev = (t2u_event_data *)arg;
    t2u_runner *runner = ev->runner_;
    t2u_context *context = ev->context_;

    (void) events;
    assert(NULL != buff);

    recv_bytes = recv(sock, buff, T2U_MESS_BUFFER_MAX, 0);
    if (recv_bytes == -1)
    {
        /* error on context's udp socket */
        LOG_(3, "recv from udp socket failed, context: %p", context);
        free(buff);
        return;
    }

    message = (t2u_message *)(void *)buff;
    message->magic_ = ntohl(message->magic_);
    message->version_ = ntohs(message->version_);
    message->oper_ = ntohs(message->oper_);
    message->handle_ = ntohl(message->handle_);
    message->seq_ = ntohl(message->seq_);

    if (((int)recv_bytes < (int)sizeof(t2u_message)) ||
        (message->magic_ != T2U_MESS_MAGIC) ||
        (message->version_ != 0x0001))
    {
        /* unknown packet */
        LOG_(2, "recv unknown packet from context: %p", context);
        unknown_callback uc = get_unknown_func_();
        if (uc)
        {
            uc(context, buff, recv_bytes);
        }
        free(buff);
        return;
    }

    switch (message->oper_)
    {
    case connect_request:
        {
            char *service_name = message->payload;
            uint32_t pair_handle = message->handle_;
            t2u_rule *rule = NULL;
            t2u_session *oldsession = NULL;

            /* check the pair_handle is already in use */
            oldsession = t2u_session_by_pair_handle(pair_handle);
            if (oldsession)
            {
                LOG_(2, "connection with remote handle is already exist.");
                
                /* close old first */
                del_forward_session(oldsession);
            }

            rule = t2u_context_find_rule(context, service_name);

            if (rule)
            {
                int ret;
                sock_t s;
                t2u_session *session;
                t2u_event_data *ev;
                struct timeval t;
                
                /* got rule */
                s = socket(AF_INET, SOCK_STREAM, 0);
                if (-1 == s)
                {
                    LOG_(3, "create socket failed");
                    free(buff);
                    return;
                }

                /* unblocking */
                evutil_make_socket_nonblocking(s);

                /* connect, async */
                ret = connect(s, (struct sockaddr *)&rule->conn_addr_, sizeof(rule->conn_addr_));
#if defined _MSC_VER
                if ((ret == -1) && (WSAGetLastError() != WSAEWOULDBLOCK))
#else
                if ((ret == -1) && (errno != EINPROGRESS))
#endif
                {
                    LOG_(3, "connect socket failed");
                    closesocket(s);
                    free(buff);
                    return;
                }

                /* new session */
                session = t2u_session_new(rule, s);
                t2u_session_assign_remote_handle(session, pair_handle);

                session->status_ = 1;

                /* add events, timer for connect timeout, EVWRITE for connect ok */
                ev = (t2u_event_data *) malloc(sizeof(t2u_event_data));
                assert(NULL != ev);

                ev->runner_ = runner;
                ev->context_ = context;
                ev->rule_ = rule;
                ev->session_ = session;
                ev->sock_ = s;
                ev->message_ = buff;

                ev->event_ = evtimer_new(runner->base_, t2u_runner_process_connect_timeout_callback, ev);
                assert (NULL != ev->event_);

                ev->extra_event_ = event_new(runner->base_, s, EV_WRITE, t2u_runner_process_connect_success_callback, ev);
                assert (NULL != ev->event_);

                t.tv_sec = (context->utimeout_ * context->uretries_) / 1000;
                t.tv_usec = ((context->utimeout_ * context->uretries_) % 1000) * 1000;

                event_add(ev->event_, &t);
                event_add(ev->extra_event_, NULL);

				t2u_rule_add_session(rule, session);
            }
        }
        break;
    case connect_response:
        {
            t2u_session *session = NULL;
            uint32_t pair_handle = 0;
            uint32_t *phandle = (void *)(message->payload);
            pair_handle = ntohl(*phandle);

            session = t2u_session_by_handle(message->handle_);
            if (session)
            {
                t2u_rule *rule = session->rule_;
                t2u_context *context = rule->context_;
                t2u_runner *runner = context->runner_;

                /* clear the timeout callback */
                if (session->timeout_ev_.event_)
                {
                    event_del(session->timeout_ev_.event_);
                    free(session->timeout_ev_.event_);
                    session->timeout_ev_.event_ = NULL;
                }

                /* success */
                if (pair_handle > 0)
                {
                    session->status_ = 2;
                    t2u_session_assign_remote_handle(session, pair_handle);
                    // t2u_rule_add_session(rule, session);
                    t2u_runner_add_session(runner, session);
                }
                else
                {
                    /* failed */
					t2u_rule_delete_session(rule, session);
                    t2u_session_delete(session);
                }
            }
            else
            {
                /* no such session, drop it */
                LOG_(3, "no such session with handle: %lu", (unsigned long) message->handle_);
            }
            free(buff);
        }
        break;
    case data_request:
        {
            char *payload = message->payload;
            int payload_len = recv_bytes - sizeof(t2u_message);
            uint32_t pair_handle = message->handle_;

            /* check the pair_handle is already in use */
            t2u_session *session = t2u_session_by_pair_handle(pair_handle);
            if (session)
            {
                /* chceck the seq */
                uint32_t incoming_seq = message->seq_;
                uint32_t ahead = incoming_seq - session->recv_seq_;
                
                if ((ahead > 0x7fffffff) || (ahead == 0))
                {
                    /* packet already proceed */
                    t2u_session_send_u_data_response(session, message, 0);
                }
                else if (ahead == 1)
                {
                    /* forward the data */
                    int sent_bytes = send(session->sock_, payload, payload_len, 0);

                    if (sent_bytes > 0)
                    {
                        t2u_session_send_u_data_response(session, message, 0);
                        /* recv increase */
                        ++session->recv_seq_;
                    }
                    else if ((int)sent_bytes == 0 || 
                             ((sent_bytes < 0) && (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)))
                    {
                        /* error: send failed */
                        LOG_(3, "send failed on socket %d, errno: %d, sent_bytes(%d) < payload_len(%d). ",
                            session->sock_, errno,
                            sent_bytes, payload_len);
                        t2u_session_send_u_data_response(session, message, 1);
                    }
                    else
                    {
                        LOG_(3, "send failed on socket %d, errno: %d, blocked ...",
                            session->sock_, errno);
                    }

                    /* TODO: check recv_mess_ */
                    while (sent_bytes > 0)
                    {
                        incoming_seq++;
                        session_message *sm = (session_message *) rbtree_lookup(session->recv_mess_, &incoming_seq);
                        if (sm)
                        {
                            /* forward the data */
                            t2u_message *nm = (t2u_message *)sm->data_;
                            sent_bytes = send(session->sock_, nm->payload, sm->len_ - sizeof(t2u_message), 0);
                            if (sent_bytes > 0)
                            {
                                t2u_session_send_u_data_response(session, nm, 0);
                                /* recv increase */
                                ++session->recv_seq_;
                            }
                            else if ((int)sent_bytes == 0 ||
                                     ((sent_bytes < 0) && (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)))
                            {
                                /* error: send failed */
                                LOG_(3, "send failed on socket %d, errno: %d, sent_bytes(%d) < payload_len(%d). ",
                                    session->sock_, errno,
                                    sent_bytes, (int)(sm->len_ - sizeof(t2u_message)));
                                t2u_session_send_u_data_response(session, nm, 1);
                            }
                            else
                            {
                                LOG_(3, "send failed on socket %d, errno: %d, blocked ...",
                                    session->sock_, errno);
                            }
                        }
                        else
                        {
                            break;
                        }
                    }
                }
                else
                {
                    /* error: lost segment */
                    t2u_session_send_u_data_response(session, message, 2);
                    // printf ("LOST SEG: %d %d \n", session->recv_seq_, incoming_seq);
                    
                    // message need to store for later process.
                    session_message *sm = (session_message *) malloc (sizeof(session_message));
                    t2u_message *nm = (t2u_message*) malloc(recv_bytes);

                    memset(sm, 0, sizeof(session_message));
                    memcpy(nm, message, recv_bytes);

                    sm->data_ = nm;
                    sm->len_ = recv_bytes;
                    sm->seq_ = sm->data_->seq_;
                    rbtree_insert(session->recv_mess_, &sm->seq_, sm);
                }
            }
            else
            {
                /* no such session */
                uint32_t *phandle = (void *)(message->payload);
                *phandle = htonl(2);
                message->oper_ = htons(data_response);

                LOG_(3, "no such session with pair handle: %lu", (unsigned long) pair_handle);

                /* send response */
                send(sock, (char *)message, sizeof(t2u_message) + sizeof(uint32_t), 0);
            }

            free(buff);
        }
        break;
    case data_response:
        {
            uint32_t *perror = (void *)message->payload;
            uint32_t error = ntohl(*perror);
            t2u_session *session = t2u_session_by_handle(message->handle_);
            if (session)
            {
                /* find the sm */
                session_message *sm = (session_message *) rbtree_lookup(session->send_mess_, &message->seq_);
                
                if (sm)
                {
                    if (error == 2)
                    {
                        uint32_t incoming_seq = message->seq_;
                        uint32_t i;;
                        /* segment lost */
                        // printf ("PORCESS LOST SEG: %d %d \n", session->send_seq_, incoming_seq);
                        for (i=incoming_seq - context->udp_slide_window_; i<incoming_seq; i++)
                        {
                            session_message *sm = (session_message *) rbtree_lookup(session->send_mess_, &incoming_seq);
                            if (sm)
                            {
                                /* send again */
                                if (sm->fast_retry_ == 0)
                                {
                                    t2u_session_send_u_mess(session, sm);
                                    sm->fast_retry_ = 1;
                                }
                                break;
                            }
                        }
                    }
                    else 
                    {
                        /* clear the timeout callback */
                        if (sm->timeout_ev_.event_)
                        {
                            event_del(sm->timeout_ev_.event_);
                            free(sm->timeout_ev_.event_);
                            sm->timeout_ev_.event_ = NULL;
                        }

                        /* confirm the data, drop the message in send buffer list */
                        {
                            rbtree_remove(session->send_mess_, &message->seq_);
                            free(sm->data_);
                            free(sm);

                            --session->send_buffer_count_;
                            // printf("buffer length: %d\n", session->send_buffer_count_);
                        }

                        /* success */
                        if (error == 0)
                        {
                            /* nothing to do */
                            /* add disable event */
                            if (session->disable_event_)
                            {
                                LOG_(1, "data confirmed, add event for session back: %p", session);
                                event_add(session->disable_event_, NULL);
                                session->disable_event_ = NULL;
                            }

                            /* check remove later and send_q */
                            if (session->remove_later_ && session->send_seq_ == message->seq_)
                            {
                                /* everythis is sent in session */
                                del_forward_session(session);
                            }
                        }
                        else
                        {
                            /* failed */
                            del_forward_session(session);
                        }
                    }
                }
                else
                {
                    LOG_(3, "no such session_message in send buffer list with seq: %lu", (unsigned long)message->seq_);
                }
            }
            else
            {
                //LOG_(3, "no such session with handle: %lu", (unsigned long)(message->handle_));
                /* no such session, drop it */
            }

            free(buff);
        }
        break;
    default:
        {
            /* unknown packet */
            LOG_(2, "recv unknown packet from context: %p, type: %d", context, ntohs(message->oper_));
            free(buff);
            return;
        }
    }

}

static void t2u_runner_process_tcp_callback(evutil_socket_t sock, short events, void *arg)
{
    t2u_event_data *ev = (t2u_event_data *)arg;
    t2u_runner *runner = ev->runner_;
    t2u_context *context = ev->context_;
    t2u_rule *rule = ev->rule_;
    t2u_session *session = ev->session_;
    char *buff = NULL;
    int read_bytes;
    struct timeval t;
    t2u_event_data *nev;

    (void)events;

    /* check session is ready for sent */
    if (session->send_buffer_count_ >= context->udp_slide_window_)
    {
        LOG_(1, "data not confirmed, disable event for session: %p", session);
        /* data is not confirmed, disable the event */
        event_del(ev->event_);
        session->disable_event_ = ev->event_;
        return;
    }

    buff = (char *)malloc(T2U_PAYLOAD_MAX);
    assert(NULL != buff);

    read_bytes = recv(sock, buff, T2U_PAYLOAD_MAX, 0);

	if (read_bytes > 0)
	{
	}
	else if ((int)read_bytes == 0 ||
		((read_bytes < 0) && (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)))
	{
		LOG_(3, "recv failed on socket %d, errno: %d, read_bytes(%d). ",
			session->sock_, errno, read_bytes);

		/* error */
		free(buff);

		/* close session */
		del_forward_session_later(session);
		return;
	}
	else
	{
		LOG_(3, "recv failed on socket %d, errno: %d, blocked ...",
			session->sock_, errno);

		free(buff);
		return;
	}

    /* send the data */
    session_message *sm = t2u_session_send_u_data(session, buff, read_bytes);
    free(buff);

    assert (NULL != sm);

    nev = &sm->timeout_ev_;
    nev->runner_ = runner;
    nev->context_ = context;
    nev->rule_ = rule;
    nev->session_ = session;
    nev->sock_ = sock;
    nev->session_message_ = sm;
    nev->event_ = evtimer_new(runner->base_, t2u_runner_process_send_timeout_callback, nev);
    
    assert (NULL != nev->event_);
    t.tv_sec = context->utimeout_ / 1000;
    t.tv_usec = (context->utimeout_ % 1000) * 1000;
    event_add(nev->event_, &t);

    /* save data for resent */
    session->send_buffer_count_++;
    rbtree_insert(session->send_mess_, &sm->seq_, sm);

    return;
}

static void t2u_runner_process_connect_timeout_callback(evutil_socket_t sock, short events, void *arg)
{
    t2u_event_data *ev = (t2u_event_data *)arg;
    /* t2u_runner *runner = ev->runner_; */
    /* t2u_context *context = ev->context_; */
	t2u_rule *rule = ev->rule_;
    t2u_session *session = ev->session_;

    (void)sock;
    (void)events;

    if (session->status_ != 2)
    {
		t2u_rule_delete_session(rule, session);

        /* not connect */
        t2u_session_delete(session);
    }

    /* cleanup, delete events and arg */
    event_del(ev->extra_event_);

    free(ev->extra_event_);
    free(ev->event_);
    free(ev->message_);
    free(ev);
}

static void t2u_runner_process_connect_success_callback(evutil_socket_t sock, short events, void *arg)
{
    int error = 0;
    unsigned int len = sizeof(int);
    t2u_event_data *ev = (t2u_event_data *)arg;
    t2u_runner *runner = ev->runner_;
    t2u_rule *rule = ev->rule_;
    t2u_session *session = ev->session_;

    (void)events;

    getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&error, &len);

    if (0 == error)
    {
        LOG_(1, "connect for session: %lu success.", (unsigned long)session->handle_);
        session->status_ = 2;

        // t2u_rule_add_session(rule, session);
        t2u_runner_add_session(runner, session);

        /* post success */
        t2u_session_send_u_connect_response(session, ev->message_);
    }
	else
	{
		LOG_(2, "connect for session: %lu failed.", (unsigned long)session->handle_);
		t2u_rule_delete_session(rule, session);
	}

    /* cleanup, delete events and arg */
    event_del(ev->event_);

    free(ev->extra_event_);
    free(ev->event_);
    free(ev->message_);
    free(ev);

}


static void t2u_runner_process_accept_callback(evutil_socket_t sock, short events, void *arg)
{
    struct sockaddr_in client_addr;
    unsigned int client_len = sizeof(client_addr); 
    t2u_event_data *ev = (t2u_event_data *)arg;
    t2u_rule *rule = (t2u_rule *)ev->rule_;
    t2u_context *context = (t2u_context *)rule->context_;
    t2u_runner *runner = (t2u_runner *)context->runner_;
    sock_t s;
    t2u_session *session;
    struct timeval t;
    t2u_event_data *nev;

    (void)sock;
    (void)events;
    
    s = accept(rule->listen_sock_, (struct sockaddr *)&client_addr, &client_len);
    if (-1 == s)
    {
        return;
    }

    /* nonblock */
    evutil_make_socket_nonblocking(s);

    /* new session, this rule must be client mode. */
    session = t2u_session_new(rule, s);
    assert(NULL != session);

    /* session is created, create a timeout timer for remove the session */
    /* send message to remote for connecting */
    nev = &session->timeout_ev_;

    nev->runner_ = runner;
    nev->context_ = context;
    nev->rule_ = rule;
    nev->session_ = session;
    nev->sock_ = s;
    nev->event_ = evtimer_new(runner->base_, t2u_runner_process_accept_timeout_callback, nev);
    assert (NULL != nev->event_);
    t.tv_sec = context->utimeout_ / 1000;
    t.tv_usec = (context->utimeout_ % 1000) * 1000;
    event_add(nev->event_, &t);

	t2u_rule_add_session(rule, session);

    /* do connect */
    t2u_session_send_u_connect(session);
}


static void t2u_runner_process_accept_timeout_callback(evutil_socket_t sock, short events, void *arg)
{
    t2u_event_data *nev = (t2u_event_data *) arg;
    t2u_session * session = nev->session_;
    t2u_context * context = nev->context_;
	t2u_rule *rule = nev->rule_;

    (void)sock;
    (void)events;

    if (++session->send_retries_ >= context->uretries_)
    {
        LOG_(2, "timeout for accept new connection, session: %lu, retry: %lu, delay: %lums",
            (unsigned long)session->handle_, context->uretries_, context->utimeout_);
        
        /* timeout */
        free(nev->event_);
        nev->event_ = NULL;

		t2u_rule_delete_session(rule, session);
        t2u_session_delete(session);
    }
    else
    {
        /* readd the timer */
        struct timeval t;
        t.tv_sec = context->utimeout_ / 1000;
        t.tv_usec = (context->utimeout_ % 1000) * 1000;
        event_add(nev->event_, &t);

        /* do connect again */
        t2u_session_send_u_connect(session);
    }
}

static void t2u_runner_process_send_timeout_callback(evutil_socket_t sock, short events, void *arg)
{
    t2u_event_data *nev = (t2u_event_data *) arg;
    t2u_session * session = nev->session_;
    t2u_context * context = nev->context_;
    session_message *sm = (session_message *)nev->session_message_;

    (void)sock;
    (void)events;
    if (++sm->send_retries_ >= context->uretries_)
    {
        LOG_(2, "timeout for send data, session: %lu, retry: %lu, delay: %lums, seq: %lu",
            (unsigned long)session->handle_, context->uretries_, context->utimeout_, (unsigned long) sm->seq_);
        
        /* timeout */
        free(nev->event_);
        nev->event_ = NULL;

        /* delete session cimplete */
        del_forward_session(session);
    }
    else
    {
        /* readd the timer */
        struct timeval t;
        t.tv_sec = context->utimeout_ / 1000;
        t.tv_usec = (context->utimeout_ % 1000) * 1000;

        event_add(nev->event_, &t);

        /* send again */
        t2u_session_send_u_mess(session, nev->session_message_);
        sm->fast_retry_ = 0;
    }
}

#define CONTROL_PORT_START (50505)
#define CONTROL_PORT_END   (50605)
/* runner init */
t2u_runner * t2u_runner_new()
{
    int ret = 0;
    struct sockaddr_in addr_c;
    unsigned short listen_port = 0;

    t2u_runner *runner = (t2u_runner *) malloc(sizeof(t2u_runner));
    assert(runner != NULL);

    /* alloc event base. */
    runner->base_ = event_base_new();
    assert(runner->base_ != NULL);
        
    /* alloc events map. */
    runner->event_tree_ = rbtree_init(NULL);
    assert(runner->event_tree_ != NULL);


    t2u_mutex_init(&runner->mutex_);
    t2u_cond_init(&runner->cond_);

    runner->running_ = 0; /* not running */
    runner->tid_ = 0;

    /* control message */
    runner->sock_[0] = socket(AF_INET, SOCK_DGRAM, 0);
    assert(runner->sock_[0] > 0);

    for (listen_port = CONTROL_PORT_START; listen_port < CONTROL_PORT_END; listen_port ++)
    {
        addr_c.sin_family = AF_INET;
        addr_c.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr_c.sin_port = htons(listen_port);

        if (bind(runner->sock_[0], (struct sockaddr *)&addr_c, sizeof(addr_c)) == -1)
        {
            LOG_(0, "socket bind failed. %s\n", strerror(errno));
        }
        else
        {
            LOG_(0, "socket bind ok on port: %u.\n", listen_port);
            break;
        }   
    }
    assert(listen_port != CONTROL_PORT_END);

    /* now connect local control server */
    runner->sock_[1] = socket(AF_INET, SOCK_DGRAM, 0);
    assert(runner->sock_[1] > 0);
    
    ret = connect(runner->sock_[1], (struct sockaddr *)&addr_c, sizeof(addr_c));
    assert(0 == ret);

    /* the event handler for control message processing. */
    runner->event_ = event_new(runner->base_, runner->sock_[0], EV_READ|EV_PERSIST, t2u_runner_control_callback, runner);
    assert(NULL != runner->event_);

    ret = event_add(runner->event_, NULL);
    assert(0 == ret);

    LOG_(0, "create new runner: %p", (void *)runner);

    /* run the runner */
    t2u_mutex_lock(&runner->mutex_);
    runner->running_ = 1;
    t2u_thr_create(&runner->thread_, t2u_runner_loop_, (void *)runner);
    t2u_cond_wait(&runner->cond_, &runner->mutex_);
    t2u_mutex_unlock(&runner->mutex_);

    return runner;
}

static void runner_delete_cb_(void *arg)
{
    t2u_runner *runner = (t2u_runner *)arg;

    /* remove self event */
    event_del(runner->event_);
    free(runner->event_);
}
    
/* destroy the runner */
void t2u_runner_delete(t2u_runner *runner)
{
    if (!runner)
    {
        return;
    }

    /* makesure stop first */
    if (runner->running_)
    {
        control_data cdata;

        /* stop all event */
        runner->running_ = 0;
        
        /* post stop control message. */
        memset(&cdata, 0, sizeof(cdata));

        cdata.func_ = runner_delete_cb_;
        cdata.arg_ = runner;

        t2u_runner_control(runner, &cdata);

        t2u_thr_join(runner->thread_);
    }

    /* cleanup */
    closesocket(runner->sock_[0]);
    closesocket(runner->sock_[1]);  
    free(runner->event_tree_);
    event_base_free(runner->base_);

    LOG_(0, "delete the runner: %p", (void *)runner);

    /* last cleanup */
    free(runner);
}


struct runner_and_context_
{
    t2u_runner *runner;
    t2u_context *context;
};

static void runner_add_context_cb_(void *arg)
{
    struct runner_and_context_ *rnc = (struct runner_and_context_ *)arg;
    t2u_runner *runner = rnc->runner;
    t2u_context *context = rnc->context;

    t2u_event_data *ev = (t2u_event_data *) malloc(sizeof(t2u_event_data));
    assert(NULL != ev);

    memset(ev, 0, sizeof(t2u_event_data));
    ev->runner_ = runner;
    ev->context_ = context;

    ev->event_ = event_new(runner->base_, context->sock_, 
        EV_READ|EV_PERSIST, t2u_runner_process_udp_callback, ev);
    assert(NULL != ev->event_);

    event_add(ev->event_, NULL);
    rbtree_insert(runner->event_tree_, ev->event_, ev);


    ((t2u_context *)ev->context_)->runner_ = runner;
    LOG_(1, "ADD_CONTEXT runner: %p context: %p", runner, context);
}

/* add context */
int t2u_runner_add_context(t2u_runner *runner, t2u_context *context)
{
    control_data cdata;
    struct runner_and_context_ rnc;

    memset(&cdata, 0, sizeof(cdata));

    cdata.func_ = runner_add_context_cb_;
    cdata.arg_ = &rnc;

    rnc.runner = runner;
    rnc.context = context;

    t2u_runner_control(runner, &cdata);
    return 0;
}


static void runner_delete_context_cb_(void *arg)
{
    struct runner_and_context_ *rnc = (struct runner_and_context_ *)arg;
    t2u_runner *runner = rnc->runner;
    t2u_context *context = rnc->context;

    t2u_event_data *ev = find_evdata_by_context(runner->event_tree_->root, context);
    assert(NULL != ev);

    rbtree_remove(runner->event_tree_, ev->event_);
    event_del(ev->event_);
    free(ev->event_);
    free (ev);

    LOG_(1, "DEL_CONTEXT runner: %p context: %p", runner, context);
}

/* delete context */
int t2u_runner_delete_context(t2u_runner *runner, t2u_context *context)
{
    control_data cdata;
    struct runner_and_context_ rnc;

    memset(&cdata, 0, sizeof(cdata));

    cdata.func_ = runner_delete_context_cb_;
    cdata.arg_ = &rnc;

    rnc.runner = runner;
    rnc.context = context;

    t2u_runner_control(runner, &cdata);
    return 0;
}

struct runner_and_rule_
{
    t2u_runner *runner;
    t2u_rule *rule;
};

void runner_add_rule_cb_(void *arg)
{
    struct runner_and_rule_ *rnr = (struct runner_and_rule_ *)arg;
    t2u_runner *runner = rnr->runner;
    t2u_rule *rule = rnr->rule;

    t2u_event_data *ev = (t2u_event_data *) malloc(sizeof(t2u_event_data));
    assert(NULL != ev);

    memset(ev, 0, sizeof(t2u_event_data));
    ev->context_ = rule->context_ ;
    ev->rule_ = rule;
    ev->runner_ = runner;

    ev->event_ = event_new(runner->base_, rule->listen_sock_, 
        EV_READ|EV_PERSIST, t2u_runner_process_accept_callback, ev);
    assert(NULL != ev->event_);

    event_add(ev->event_, NULL);
    rbtree_insert(runner->event_tree_, ev->event_, ev);

    LOG_(1, "ADD_RULE runner: %p rule: %p", runner, rule);
}

/* add rule */
int t2u_runner_add_rule(t2u_runner *runner, t2u_rule *rule)
{
    if (rule->mode_ == forward_client_mode)
    {
        control_data cdata;
        struct runner_and_rule_ rnr;

        memset(&cdata, 0, sizeof(cdata));

        rnr.rule = rule;
        rnr.runner = runner;

        cdata.func_ = runner_add_rule_cb_;
        cdata.arg_ = &rnr;

        t2u_runner_control(runner, &cdata);
    }
    return 0;
}

void runner_delete_rule_cb_(void *arg)
{
    struct runner_and_rule_ *rnr = (struct runner_and_rule_ *)arg;
    t2u_runner *runner = rnr->runner;
    t2u_rule *rule = rnr->rule;

    t2u_event_data *ev = find_evdata_by_rule(runner->event_tree_->root, rule);
    assert(NULL != ev);

    rbtree_remove(runner->event_tree_, ev->event_);
    event_del(ev->event_);
    free(ev->event_);
    free (ev);

    LOG_(1, "DEL_RULE runner: %p rule: %p", runner, rule);
}

/* delete rule */
int t2u_runner_delete_rule(t2u_runner *runner, t2u_rule *rule)
{
    if (rule->mode_ == forward_client_mode)
    {    
        control_data cdata;
        struct runner_and_rule_ rnr;

        memset(&cdata, 0, sizeof(cdata));

        rnr.rule = rule;
        rnr.runner = runner;

        cdata.func_ = runner_delete_rule_cb_;
        cdata.arg_ = &rnr;

        t2u_runner_control(runner, &cdata);
    }
    return 0;
}

struct runner_and_session_
{
    t2u_runner *runner;
    t2u_session *session;
};

void runner_add_session_cb_(void *arg)
{
    struct runner_and_session_ *rns = (struct runner_and_session_ *)arg;
    t2u_runner *runner = rns->runner;
    t2u_session *session = rns->session;
    t2u_rule *rule = session->rule_;
    t2u_context *context = rule->context_;


    t2u_event_data *ev = (t2u_event_data *) malloc(sizeof(t2u_event_data));
    assert(NULL != ev);

    memset(ev, 0, sizeof(t2u_event_data));
    
    ev->session_ = session;
    ev->rule_ = rule;
    ev->context_ = context;
    ev->runner_ = runner;

    ev->event_ = event_new(runner->base_, session->sock_, 
    EV_READ|EV_PERSIST, t2u_runner_process_tcp_callback, ev);
    assert(NULL != ev->event_);

    event_add(ev->event_, NULL);
    rbtree_insert(runner->event_tree_, ev->event_, ev);

    LOG_(1, "ADD_SESSION runner: %p session: %p, sock: %d", runner, session, (int)session->sock_);
}

/* add session */
int t2u_runner_add_session(t2u_runner *runner, t2u_session *session)
{
    control_data cdata;
    struct runner_and_session_ rns;
    
    memset(&cdata, 0, sizeof(cdata));

    rns.runner = runner;
    rns.session = session;

    cdata.func_ = runner_add_session_cb_;
    cdata.arg_ = &rns;

    t2u_runner_control(runner, &cdata);
    
    return 0;
}

void runner_delete_session_cb_(void *arg)
{
    struct runner_and_session_ *rns = (struct runner_and_session_ *)arg;
    t2u_runner *runner = rns->runner;
    t2u_session *session = rns->session;

    t2u_event_data *ev = find_evdata_by_session(runner->event_tree_->root, session);
    if (ev)
    {
        rbtree_remove(runner->event_tree_, ev->event_);
        event_del(ev->event_);
        free(ev->event_);
        free (ev);
    }

    LOG_(1, "DEL_SESSION runner: %p session: %p", runner, session);
}

/* delete session */
int t2u_runner_delete_session(t2u_runner *runner, t2u_session *session)
{
    control_data cdata;
    struct runner_and_session_ rns;

    memset(&cdata, 0, sizeof(cdata));

    rns.runner = runner;
    rns.session = session;

    cdata.func_ = runner_delete_session_cb_;
    cdata.arg_ = &rns;

    t2u_runner_control(runner, &cdata);
    
    return 0;
}


/* runner proc */
#if defined __GNUC__
    static void* t2u_runner_loop_(void *arg)
#elif defined _MSC_VER
    static DWORD __stdcall t2u_runner_loop_(void * arg)
#endif
{
    t2u_runner *runner = (t2u_runner *)arg;
    runner->tid_ = t2u_thr_self();

    t2u_mutex_lock(&runner->mutex_);
    t2u_cond_signal(&runner->cond_);
    t2u_mutex_unlock(&runner->mutex_);

    LOG_(0, "enter run loop for runner: %p", (void *)runner);
    
    /*  run loop */
    event_base_dispatch(runner->base_);

    LOG_(0, "end run loop for runner: %p", (void *)runner);
#if defined __GNUC__
    return NULL;
#elif defined _MSC_VER
    return 0;
#endif
}
