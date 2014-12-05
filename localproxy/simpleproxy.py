#!/usr/bin/python3

# Copyright (C) 2013 Niall McCarroll
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, 
# distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
# following conditions:
#
# The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

# simpleproxy.py
# a very simple proxy server written in python2/python3
# run python|python3 simpleproxy.py proxyhost:proxyport serverhost:serverport

import select
import socket
import sys
import os
import fcntl
import logging

USAGE = "usage: python simpleproxy.py proxyhost:proxyport desthost:destport"

class ProxyConnection(object):

    # enable a buffer on connections with this many bytes
    MAX_BUFFER_SIZE = 1024

    # ProxyConnection class forwards data between a client and a destination socket

    def __init__(self,proxyserver,listensock,servaddr):
        self.proxyserver = proxyserver
        self.servaddr = servaddr    # the server address

        # open client and server sockets
        self.clisock, self.cliaddr = listensock.accept() # client socket and address
        self.clisock.setblocking(0)             
        self.servsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM) # server socket
        self.servsock.setblocking(0)
        
        # buffers for data recieved from a socket
        self.buffers = { self.clisock:bytes(), self.servsock:bytes() }           
        
        self.connected = False      # is the server socket connected yet

        # register sockets with server and enable read operations
        self.proxyserver.registerSocket(self.clisock,self)
        self.proxyserver.registerSocket(self.servsock,self)
        self.proxyserver.activateRead(self.clisock)
        self.proxyserver.activateRead(self.servsock)

    # return the socket on the "other end" of the connection
    def other(self,socket):
        if socket == self.clisock:
            return self.servsock
        else:
            return self.clisock

    # connect to the server connection
    def connect(self):
        # have to use socket's connect_ex because the connect is asynchronous and won't suceed immediately
        self.servsock.connect_ex(self.servaddr)

    # read data in from a socket
    def readfrom(self,s):
        # is the connection being opened by the server responding to the connect?
        if s == self.servsock and not self.connected:
            self.proxyserver.connection_count += 1
            logging.getLogger("simpleproxy") \
                .info("opened connection from %s, connection count now %d"%(str(self.cliaddr),self.proxyserver.connection_count))
            self.connected = True
            return
    
        # read from the socket
        capacity = ProxyConnection.MAX_BUFFER_SIZE - len(self.buffers[s])

        try:
            data = s.recv(capacity)
        except Exception as ex:
            data = ""

        # if the read failed, close the socket (this happens when the client or server closes the connection)
        if len(data) == 0:
            self.close()
            return

        # buffer the read data 
        self.buffers[s] += data
        self.proxyserver.activateWrite(self.other(s))

        # disable further reads if buffer is full
        capacity -= len(data)
        if capacity <= 0:
            self.proxyserver.deactivateRead(s)

    
    # write data out to a socket
    def writeto(self,s):
        # get the buffer containing data to be read
        buf = self.buffers[self.other(s)]

        # write it to the socket
        written = s.send(buf)
        
        # remove written data from the buffer
        buf = buf[written:]
        self.buffers[self.other(s)] = buf

        # disable further writes if the buffer is empty
        if len(buf) == 0:
            self.proxyserver.deactivateWrite(s)  
        # enable reads if data was written
        if written:
            self.proxyserver.activateRead(self.other(s))
        

    # close the connection sockets
    def close(self):
        for sock in [self.clisock,self.servsock]:
            if sock:
                self.proxyserver.deactivateRead(sock)
                self.proxyserver.deactivateWrite(sock)       
                sock.close()
                self.proxyserver.unregisterSocket(sock,self)
        
        self.proxyserver.connection_count -= 1
        logging.getLogger("simpleproxy") \
                .info("closed connection from %s, connection count now %d"%(self.cliaddr,self.proxyserver.connection_count))

 
class ProxyServer(object):

    def __init__(self,host,port,serverhost,serverport):
        self.address = (host,port)
        self.server = (serverhost,serverport)
        self.listensock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listensock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listensock.bind(self.address)
        self.listensock.listen(5)
        self.connections = {}               # map from a socket to a ProxyConnection
        self.readsockets = []               # all sockets which can be read
        self.writesockets = []              # all sockets which can be written
        self.allsockets = [self.listensock] # all opened sockets
        self.connection_count = 0           # count of all active connections
        
        
    def run(self):
        loop = 0
        while True:
            # block until there is some activity on one of the sockets, timeout every 60 seconds by default
            r, w, e = select.select(
                            [self.listensock]+self.readsockets, 
                            self.writesockets, 
                            self.allsockets,                            
                            60)
            loop += 1
            # handle any reads            
            for s in r:
                if s is self.listensock:
                    # open a new connection
                    self.open()
                else:
                    if s in self.connections:
                        self.connections[s].readfrom(s)
            # handle any writes
            for s in w:
                if s in self.connections:
                    self.connections[s].writeto(s)
            # handle errors (close connections)
            for s in e:
                if s in self.connections:
                    self.connections[s].close()
  
        self.sock.close()
        self.sock = None

    def activateRead(self,sock):
        if not sock in self.readsockets:
            self.readsockets.append(sock)

    def deactivateRead(self,sock):
        if sock in self.readsockets:
            self.readsockets.remove(sock)

    def activateWrite(self,sock):
        if not sock in self.writesockets:
            self.writesockets.append(sock)

    def deactivateWrite(self,sock):
        if sock in self.writesockets:
            self.writesockets.remove(sock)

    def registerSocket(self,sock,conn):
        self.connections[sock] = conn
        self.allsockets.append(sock)

    def unregisterSocket(self,sock,conn):
        del self.connections[sock]
        self.allsockets.remove(sock)

    # open a new proxy connection from the listening socket
    def open(self):    
        conn = ProxyConnection(self,self.listensock,self.server)
        conn.connect()

if __name__ == '__main__':
    try:
        proxy = sys.argv[1].split(":")
        dest = sys.argv[2].split(":")
        proxyhost = proxy[0]
        proxyport = int(proxy[1])
        serverhost = dest[0]
        serverport = int(dest[1])
    except:    
        print(USAGE)
        sys.exit(-1)

    logger = logging.getLogger('simpleproxy')
    logger.setLevel(logging.INFO)
    hdlr = logging.StreamHandler()
    hdlr.setLevel(logging.INFO)
    hdlr.setFormatter(logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s'))
    logger.addHandler(hdlr)

    server = ProxyServer(proxyhost,proxyport,serverhost,serverport)
    server.run()



