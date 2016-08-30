/*
 * Copyright (C) 2016 Daniel Nicoletti <dantti12@gmail.com>
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
#include "tcpserver.h"
#include "socket.h"
#include "protocolhttp.h"

#include <Cutelyst/Engine>
#include <QDateTime>
#include <QSocketNotifier>

using namespace CWSGI;

TcpServer::TcpServer(QObject *parent) : QTcpServer(parent)
{
    m_proto = new ProtocolHttp(this);
    m_engine = qobject_cast<CWsgiEngine*>(parent);
}

void TcpServer::incomingConnection(qintptr handle)
{
//    qDebug() << Q_FUNC_INFO << handle << thread();
    TcpSocket *sock;
    if (m_socks.size()) {
        sock = m_socks.back();
        m_socks.pop_back();
        sock->resetSocket();
    } else {
        sock = new TcpSocket(this);
        sock->engine = m_engine;
        static QString serverAddr = serverAddress().toString();
        sock->serverAddress = serverAddr;
        connect(sock, &QIODevice::readyRead, m_proto, &Protocol::readyRead);
        connect(sock, &TcpSocket::finished, this, &TcpServer::enqueue);
    }

//    auto requestNotifier = new QSocketNotifier(handle, QSocketNotifier::Read, sock);
//    connect(requestNotifier, &QSocketNotifier::activated, sock, &QIODevice::readyRead);
//sock->fd = handle;
//    connect(requestNotifier, &QSocketNotifier::activated,
//            [=]() {
//        sock->readyRead();
//    });

//    qDebug() << Q_FUNC_INFO << handle << sock;

    sock->setSocketDescriptor(handle);
    sock->start = QDateTime::currentMSecsSinceEpoch();

//    auto server = qobject_cast<QTcpServer*>(sender());
//    QTcpSocket *conn = server->nextPendingConnection();
//    if (conn) {
//    connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
//    connect(sock, &QTcpSocket::disconnected, this, &TcpServer::enqueue);
//        TcpSocket *sock = qobject_cast<TcpSocket*>(conn);
//        sock->engine = m_engine;
//        static QString serverAddr = serverAddress().toString();
//        sock->serverAddress = serverAddr;
//        connect(sock, &QIODevice::readyRead, m_proto, &Protocol::readyRead);
//    }
        //    addPendingConnection(sock);
}

void TcpServer::enqueue()
{
    m_socks.push_back(qobject_cast<TcpSocket*>(sender()));
}

#include "moc_tcpserver.cpp"
