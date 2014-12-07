#!/usr/bin/env node

var WebSocketClient = require('websocket').client;
var events = require('events');
var querystring = require('querystring');
var http = require('http');
var fs = require('fs');
var util = require('util');
var net = require("net");

if (typeof String.prototype.startsWith != 'function') {
  String.prototype.startsWith = function (str){
    return this.slice(0, str.length) == str;
  };
}
if (typeof String.prototype.endsWith !== 'function') {
    String.prototype.endsWith = function(suffix) {
        return this.indexOf(suffix, this.length - suffix.length) !== -1;
    };
}

//var eventEmitter = new events.EventEmitter();
if(process.argv.length != 3) {
    console.log("require a file of config.json");
    process.exit();
}
var args = {
    secure: false,
    version: 13
};

fileName = process.argv[2];
var fileBuf = fs.readFileSync(fileName, "utf8");
var args = JSON.parse(fileBuf.toString());

args.protocol = args.secure ? 'wss:' : 'ws:'

function do_request(options, post_data, callback) {
    var req = http.request(options, function(res) {
        res.setEncoding('utf8');
        var data = ""
        res.on('data', function (chunk) {
            data += chunk;
        });
        res.on('end', function() {
            callback(res, data);
        })
    });

    // post the data
    if(options.method == 'POST') {
        req.write(post_data);
    }
    req.end();
}

var remoteAccessor = ( function () {
    function Accessor(host, port) {
        this.cookie = '';
        this.device = '';
        this.seq = '';
        events.EventEmitter.call(this);
    }
    util.inherits(Accessor, events.EventEmitter);

    Accessor.prototype.loginUser = function(username, password) {
        // Build the post string from an object
        var post_data = querystring.stringify({
            'username' : username,
            'password': password
        });

        // An object of options to indicate where to post to
        var post_options = {
            host: args.host,
            port: args.port,
            path: '/__login',
            method: 'POST',
            headers: {
                'Content-Type': 'application/x-www-form-urlencoded',
                'Content-Length': post_data.length
            }
        };

        var self = this;
        do_request(post_options, post_data, function (res, data) {
            cookie = res.headers["set-cookie"];
            if(typeof cookie == "undefined") {
                self.emit("failed");
            }
            else {
                self.cookie = cookie[0];
                console.log(self.cookie);
                self.emit("loginOk");
            }
        });
    }

    Accessor.prototype.getDevices = function () {
        var self = this;
        var options = {
            host: args.host,
            port: args.port,
            path: '/__list',
            method: 'GET',
            headers: {
                cookie: self.cookie
            }
        };
        do_request(options, "", function (res, data) {
            //console.log(data);
            self.emit("gotDevice", data);
        });
    }

    Accessor.prototype.requestDevice = function(device) {
        var self = this;
        var options = {
            host: args.host,
            port: args.port,
            path: '/__device/'+device,
            method: 'GET',
            headers: {
                cookie: self.cookie
            }
        };
        do_request(options, "", function (res, data) {
            //cookie = res.headers["set-cookie"];
            //console.log(cookie);
            self.emit("telnet", data);
        });
    }

    return Accessor;
})();

/* var forwardService = (function() {
    function Service() {
        this.ws_connected = false;
        this.proxy_connected = false;
        events.EventEmitter.call(this);
    }
    util.inherits(Accessor, events.EventEmitter);

    return Service;
})(); */

var remoteAcc = new remoteAccessor(args.host, args.port);
remoteAcc.on('failed', function(){
    console.log("access failed");
});
remoteAcc.on('loginOk', function() {
    console.log("loginOk");
    this.getDevices();
});
remoteAcc.on('gotDevice', function(data) {
    var obj = JSON.parse(data);
    if(obj.devices.length <= 0) {
        console.log("not devices");
        return;
    }
    console.log(obj.devices[0]);
    remoteAcc.requestDevice(obj.devices[0]);

});

function proxyService(proxySocket) {
    //new client coming
    var connected = false;
    var buffers = new Array();
    var the_seq = 0;
    var buf_failed = new Buffer("CLOSED!").toString("base64").replace("==", "");

    var localWs = new WebSocketClient({
        webSocketVersion: args.version
    });

    localWs.on('connectFailed', function(error) {
        console.log("Connect Error: " + error.toString());
        proxySocket.end();
        proxySocket.close();
    });

    localWs.on('connect', function(connection) {
        console.log("socket connected");

        //Do handshake
        connection.send("hello");

        connection.on('error', function(error) {
            console.log("Connection Error: " + error.toString());
            proxySocket.end();
        });
        connection.on('close', function() {
            console.log("the connection is closed");
            proxySocket.end();
        })
        connection.on('message', function(msg) {
            var obj = JSON.parse(msg.utf8Data);
            if(obj.type == "handshake") {
                var newObj = {};
                newObj.seq = obj.seq;
                newObj.type = "openning";
                newObj.message = args.lan_host + ":" + args.lan_port;
                the_seq = obj.seq;
                connection.send(JSON.stringify(newObj));
                return;
            }

            //Forward buffer to local socket
            if(!connected) {
                if(buffers.length > 0) {
                    var total_buf = Buffer.concat(buffers);
                    var newObj = {};
                    newObj.seq = the_seq;
                    newObj.type = "request";
                    newObj.message = total_buf.toString("base64");
                    connection.send(JSON.stringify(newObj));
                    buffers = [];
                }
                connected = true;
            }

            if((obj.message.length >= buf_failed.length)
               && (obj.message.startsWith(buf_failed))) {
                    console.log("exit");
                    proxySocket.end();
                    connection.close();
                    return;
            }
            var buf = new Buffer(obj.message, "base64");
            proxySocket.write(buf);
        });

        proxySocket.on("error", function (e) {
            console.log("client closed");
            connection.close();
        });
        proxySocket.on("data", function (data){
            if(connected) {
                // data from local socket, forward to ws
                var newObj = {};
                newObj.seq = the_seq;
                newObj.type = "request";
                newObj.message = data.toString("base64");
                connection.send(JSON.stringify(newObj));
            }
            else {
                buffers[buffers.length] = data;
            }
        });

    });

    localWs.connect(args.protocol + '//' + args.host + ':' + args.port + '/'
        , 'dumb-increment-protocol', ""
        , {
            "cookie":remoteAcc.cookie
            }
    );
}

remoteAcc.on('telnet', function (){
    net.createServer(proxyService).listen(args.local_port);
});

//Start login now
remoteAcc.loginUser(args.username, args.password);

