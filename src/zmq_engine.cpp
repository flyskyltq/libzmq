/*
    Copyright (c) 2007-2011 iMatix Corporation
    Copyright (c) 2007-2011 Other contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "platform.hpp"
#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#endif

#include <string.h>
#include <new>

#include "zmq_engine.hpp"
#include "zmq_connecter.hpp"
#include "io_thread.hpp"
#include "session.hpp"
#include "config.hpp"
#include "err.hpp"

zmq::zmq_engine_t::zmq_engine_t (fd_t fd_, const options_t &options_) :
    inpos (NULL),
    insize (0),
    decoder (in_batch_size, options_.maxmsgsize),
    outpos (NULL),
    outsize (0),
    encoder (out_batch_size),
    session (NULL),
    leftover_session (NULL),
    options (options_),
    plugged (false)
{
    //  Initialise the underlying socket.
    int rc = tcp_socket.open (fd_, options.sndbuf, options.rcvbuf);
    zmq_assert (rc == 0);
}

zmq::zmq_engine_t::~zmq_engine_t ()
{
    zmq_assert (!plugged);
}

void zmq::zmq_engine_t::plug (io_thread_t *io_thread_, session_t *session_)
{
    zmq_assert (!plugged);
    plugged = true;
    leftover_session = NULL;

    //  Connect to session object.
    zmq_assert (!session);
    zmq_assert (session_);
    encoder.set_session (session_);
    decoder.set_session (session_);
    session = session_;

    //  Connect to I/O threads poller object.
    io_object_t::plug (io_thread_);
    handle = add_fd (tcp_socket.get_fd ());
    set_pollin (handle);
    set_pollout (handle);

    //  Flush all the data that may have been already received downstream.
    in_event ();
}

void zmq::zmq_engine_t::unplug ()
{
    zmq_assert (plugged);
    plugged = false;

    //  Cancel all fd subscriptions.
    rm_fd (handle);

    //  Disconnect from I/O threads poller object.
    io_object_t::unplug ();

    //  Disconnect from session object.
    encoder.set_session (NULL);
    decoder.set_session (NULL);
    leftover_session = session;
    session = NULL;
}

void zmq::zmq_engine_t::terminate ()
{
    unplug ();
    delete this;
}

void zmq::zmq_engine_t::in_event ()
{
    bool disconnection = false;

    //  If there's no data to process in the buffer...
    if (!insize) {

        //  Retrieve the buffer and read as much data as possible.
        //  Note that buffer can be arbitrarily large. However, we assume
        //  the underlying TCP layer has fixed buffer size and thus the
        //  number of bytes read will be always limited.
        decoder.get_buffer (&inpos, &insize);
        insize = tcp_socket.read (inpos, insize);

        //  Check whether the peer has closed the connection.
        if (insize == (size_t) -1) {
            insize = 0;
            disconnection = true;
        }
    }

    //  Push the data to the decoder.
    size_t processed = decoder.process_buffer (inpos, insize);

    if (unlikely (processed == (size_t) -1)) {
        disconnection = true;
    }
    else {

        //  Stop polling for input if we got stuck.
        if (processed < insize) {

            //  This may happen if queue limits are in effect.
            if (plugged)
                reset_pollin (handle);
        }

        //  Adjust the buffer.
        inpos += processed;
        insize -= processed;
    }

    //  Flush all messages the decoder may have produced.
    //  If IO handler has unplugged engine, flush transient IO handler.
    if (unlikely (!plugged)) {
        zmq_assert (leftover_session);
        leftover_session->flush ();
    } else {
        session->flush ();
    }

    if (session && disconnection)
        error ();
}

void zmq::zmq_engine_t::out_event ()
{
    //  If write buffer is empty, try to read new data from the encoder.
    if (!outsize) {

        outpos = NULL;
        encoder.get_data (&outpos, &outsize);

        //  If IO handler has unplugged engine, flush transient IO handler.
        if (unlikely (!plugged)) {
            zmq_assert (leftover_session);
            leftover_session->flush ();
            return;
        }

        //  If there is no data to send, stop polling for output.
        if (outsize == 0) {
            reset_pollout (handle);
            return;
        }
    }

    //  If there are any data to write in write buffer, write as much as
    //  possible to the socket. Note that amount of data to write can be
    //  arbitratily large. However, we assume that underlying TCP layer has
    //  limited transmission buffer and thus the actual number of bytes
    //  written should be reasonably modest.
    int nbytes = tcp_socket.write (outpos, outsize);

    //  Handle problems with the connection.
    if (nbytes == -1) {
        error ();
        return;
    }

    outpos += nbytes;
    outsize -= nbytes;
}

void zmq::zmq_engine_t::activate_out ()
{
    set_pollout (handle);

    //  Speculative write: The assumption is that at the moment new message
    //  was sent by the user the socket is probably available for writing.
    //  Thus we try to write the data to socket avoiding polling for POLLOUT.
    //  Consequently, the latency should be better in request/reply scenarios.
    out_event ();
}

void zmq::zmq_engine_t::activate_in ()
{
    set_pollin (handle);

    //  Speculative read.
    in_event ();
}

void zmq::zmq_engine_t::error ()
{
    zmq_assert (session);
    session->detach ();
    unplug ();
    delete this;
}
