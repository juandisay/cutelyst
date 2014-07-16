/*
 * Copyright (C) 2013-2014 Daniel Nicoletti <dantti12@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB. If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "engineuwsgi.h"
#include "plugin.h"

#include <QCoreApplication>
#include <QSocketNotifier>

#define free_req_queue uwsgi.async_queue_unused_ptr++; uwsgi.async_queue_unused[uwsgi.async_queue_unused_ptr] = wsgi_req

using namespace Cutelyst;

static EngineUwsgi *engine;

void cuteOutput(QtMsgType, const QMessageLogContext &, const QString &);
void uwsgi_cutelyst_loop(void);

/**
 * This function is called as soon as
 * the plugin is loaded
 */
extern "C" void uwsgi_cutelyst_on_load()
{
    uwsgi_register_loop( (char *) "CutelystQtLoop", uwsgi_cutelyst_loop);

    (void) new QCoreApplication(uwsgi.argc, uwsgi.argv);

    qInstallMessageHandler(cuteOutput);
}

extern "C" int uwsgi_cutelyst_init()
{
    qCDebug(CUTELYST_UWSGI) << "Initializing Cutelyst plugin";
    qCDebug(CUTELYST_UWSGI) << "-> async" << uwsgi.async << "-> threads" << uwsgi.threads;
    if (uwsgi.async < uwsgi.threads) {
        uwsgi_log("--async must be greater or equal to --threads value\n");
        exit(1);
    }

    uwsgi.loop = (char *) "CutelystQtLoop";

    return 0;
}

extern "C" void uwsgi_cutelyst_post_fork()
{
    if (!engine->postFork()) {
        qCCritical(CUTELYST_UWSGI) << "Could not setup application on post fork";

#ifdef UWSGI_GO_CHEAP_CODE
        // We need to tell the master process that the
        // application failed to setup and that it shouldn't
        // try to respawn the worker
        exit(UWSGI_GO_CHEAP_CODE);
#endif // UWSGI_GO_CHEAP_CODE
    }
}

extern "C" int uwsgi_cutelyst_request(struct wsgi_request *wsgi_req)
{
    // empty request ?
    if (!wsgi_req->uh->pktsize) {
        qCDebug(CUTELYST_UWSGI) << "Empty request. skip.";
        return -1;
    }

    // get uwsgi variables
    if (uwsgi_parse_vars(wsgi_req)) {
        qCDebug(CUTELYST_UWSGI) << "Invalid request. skip.";
        return -1;
    }

    engine->processRequest(wsgi_req);

    return UWSGI_OK;
}

#ifdef UWSGI_GO_CHEAP_CODE // Actually we only need uwsgi 2.0.1
static void fsmon_reload(struct uwsgi_fsmon *fs)
{
    qCDebug(CUTELYST_UWSGI) << "Reloading application due to file change";
    uwsgi_reload(uwsgi.argv);
}
#endif // UWSGI_GO_CHEAP_CODE

/**
 * This function is called when the master process is exiting
 */
extern "C" void uwsgi_cutelyst_master_cleanup()
{
    qCDebug(CUTELYST_UWSGI) << "Master process finishing" << QCoreApplication::applicationPid();
    delete qApp;
    qCDebug(CUTELYST_UWSGI) << "Master process finished" << QCoreApplication::applicationPid();
}

/**
 * This function is called when the child process is exiting
 */
extern "C" void uwsgi_cutelyst_atexit()
{
    qCDebug(CUTELYST_UWSGI) << "Child process finishing" << QCoreApplication::applicationPid();
    delete engine;
    qCDebug(CUTELYST_UWSGI) << "Child process finished" << QCoreApplication::applicationPid();
}

extern "C" void uwsgi_cutelyst_init_apps()
{
    qCDebug(CUTELYST_UWSGI) << "Cutelyst Init App";

    QString path(options.app);
    if (path.isEmpty()) {
        qCCritical(CUTELYST_UWSGI) << "Cutelyst Application name or path was not set";
        return;
    }

#ifdef UWSGI_GO_CHEAP_CODE
    if (options.reload) {
        // Register application auto reload
        char *file = qstrdup(path.toUtf8().constData());
        uwsgi_register_fsmon(file, fsmon_reload, NULL);
    }
#endif // UWSGI_GO_CHEAP_CODE

    QString config(options.config);
    if (!config.isNull()) {
        qputenv("CUTELYST_CONFIG", config.toUtf8());
    }

    engine = new EngineUwsgi(qApp);

    qCDebug(CUTELYST_UWSGI) << "Loading" << path;
    if (!engine->loadApplication(path)) {
        qCCritical(CUTELYST_UWSGI) << "Could not load application:" << path;
        return;
    }

    // register a new app under a specific "mountpoint"
    uwsgi_add_app(1, CUTELYST_MODIFIER1, NULL, 0, NULL, NULL);
}

void uwsgi_cutelyst_watch_signal(int signalFD)
{
    QSocketNotifier *socketNotifier = new QSocketNotifier(signalFD, QSocketNotifier::Read);
    QObject::connect(socketNotifier, &QSocketNotifier::activated,
                     [=](int fd) {
        uwsgi_receive_signal(fd, (char *) "worker", uwsgi.mywid);
    });
}

void uwsgi_cutelyst_watch_request(struct uwsgi_socket *uwsgi_sock)
{
    QSocketNotifier *socketNotifier = new QSocketNotifier(uwsgi_sock->fd, QSocketNotifier::Read);
    QObject::connect(socketNotifier, &QSocketNotifier::activated,
                     [=](int fd) {
        struct wsgi_request *wsgi_req = find_first_available_wsgi_req();
        if (wsgi_req == NULL) {
            uwsgi_async_queue_is_full(uwsgi_now());
            return;
        }

        // fill wsgi_request structure
        wsgi_req_setup(wsgi_req, wsgi_req->async_id, uwsgi_sock);

        qCDebug(CUTELYST_UWSGI) << "wsgi_req->async_id" << wsgi_req->async_id << fd;
        qCDebug(CUTELYST_UWSGI) << "in_request" << uwsgi.workers[uwsgi.mywid].cores[wsgi_req->async_id].in_request;

        // mark core as used
        uwsgi.workers[uwsgi.mywid].cores[wsgi_req->async_id].in_request = 1;

        // accept the connection
        if (wsgi_req_simple_accept(wsgi_req, fd)) {
            uwsgi.workers[uwsgi.mywid].cores[wsgi_req->async_id].in_request = 0;
            free_req_queue;
            return;
        }

        wsgi_req->start_of_request = uwsgi_micros();
        wsgi_req->start_of_request_in_sec = wsgi_req->start_of_request/1000000;

        // enter harakiri mode
        if (uwsgi.harakiri_options.workers > 0) {
            set_harakiri(uwsgi.harakiri_options.workers);
        }
        qCDebug(CUTELYST_UWSGI) << "in_request" << uwsgi.workers[uwsgi.mywid].cores[wsgi_req->async_id].in_request;


        for(;;) {
            int ret = uwsgi_wait_read_req(wsgi_req);

            if (ret <= 0) {
                goto end;
            }

            int status = wsgi_req->socket->proto(wsgi_req);
            if (status < 0) {
                goto end;
            } else if (status == 0) {
                break;
            }
        }

        qCDebug(CUTELYST_UWSGI) << "async_environ" << wsgi_req->async_environ;

        uwsgi_cutelyst_request(wsgi_req);

end:
        uwsgi_close_request(wsgi_req);
        free_req_queue;
    });
}

void uwsgi_cutelyst_loop()
{
    qDebug(CUTELYST_UWSGI) << "Using Cutelyst Qt Loop";

    // ensure SIGPIPE is ignored
    signal(SIGPIPE, SIG_IGN);

    // FIX for some reason this is not being set by UWSGI
    uwsgi.wait_read_hook = uwsgi_simple_wait_read_hook;

    if (!uwsgi.async_waiting_fd_table) {
        uwsgi.async_waiting_fd_table = static_cast<wsgi_request **>(uwsgi_calloc(sizeof(struct wsgi_request *) * uwsgi.max_fd));
    }

    if (!uwsgi.async_proto_fd_table) {
        uwsgi.async_proto_fd_table = static_cast<wsgi_request **>(uwsgi_calloc(sizeof(struct wsgi_request *) * uwsgi.max_fd));
    }

    // monitor signals
    if (uwsgi.signal_socket > -1) {
        uwsgi_cutelyst_watch_signal(uwsgi.signal_socket);
        uwsgi_cutelyst_watch_signal(uwsgi.my_signal_socket);
    }

    // monitor sockets
    struct uwsgi_socket *uwsgi_sock = uwsgi.sockets;
    while(uwsgi_sock) {
        uwsgi_cutelyst_watch_request(uwsgi_sock);
        uwsgi_sock = uwsgi_sock->next;
    }

    // start the qt event loop
    qApp->exec();
}

void cuteOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QByteArray localMsg = msg.toLocal8Bit();
    switch (type) {
    case QtDebugMsg:
        uwsgi_log("%s[debug] %s\n", context.category, localMsg.constData());
        break;
    case QtWarningMsg:
        uwsgi_log("%s[warn] %s\n", context.category, localMsg.constData());
        break;
    case QtCriticalMsg:
        uwsgi_log("%s[crit] %s\n", context.category, localMsg.constData());
        break;
    case QtFatalMsg:
        uwsgi_log("%s[fatal] %s\n", context.category, localMsg.constData());
        abort();
    }
}
