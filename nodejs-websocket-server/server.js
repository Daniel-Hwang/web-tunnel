var WebSocketServer = require('websocket').server;
var WebSocketRouter = require('websocket').router;
var express = require('express');
//var session = require('express-session')
var request = require('request');
var concat = require('concat-stream');
var HashMap = require('hashmap').HashMap;
var buffertools = require('buffertools');
var stream = require("stream");
var Streamifier = require('streamifier');
var fileSystem = require('fs');
var path = require('path');
var querystring = require("querystring");

var cfg = {
    ssl: false,
    port: 8060,
    ssl_key: '/home/janson/projects/nodejs/ws/examples/tmp/privatekey.pem',
    ssl_cert: '/home/janson/projects/nodejs/ws/examples/tmp/certificate.pem'
};

var http = null;
var app = null;
if(cfg.ssl) {
    http = require("https");
    app = express.createServer({
        key: fileSystem.readFileSync( cfg.ssl_key ),
        cert: fileSystem.readFileSync( cfg.ssl_cert )
    });
} else {
    http = require("http");
    app = express.createServer();
}

var userMap = new HashMap();
var userMgrMax = 512;
var protocolHeaderLen = 16;

//负责处理Web请求
var httpProcessor = (function () {
    function HttpProcessor(req, res, seq) {
        this.req = req;
        this.res = res;
        this.seq = seq;
        this.headerOk = false;
        this.headerStr = "";
        this.start = 0;
        this.jsInsert = false;
        this.jsStr = "";
    }
    HttpProcessor.prototype = {
        constructor:HttpProcessor,
        innerProcessBuffer: function(mgr, buffer) {
            if((!this.jsInsert) && (this.req.url == '/')) {
                this.toInsertJs(mgr, buffer);
            }
            else {
                this.res.write(buffer);
            }
        },
        processBuffer: function(mgr, buffer) {
            this.start += buffer.length;

            if(!this.headerOk) {
                this.processHeader(mgr, buffer);
            }
            else {
                this.innerProcessBuffer(mgr, buffer);
            }

            if(this.start >= mgr.curr_total) {
                this.res.end();
                mgr.delSeq(mgr.curr_seq);
                mgr.curr_seq = null;
                mgr.curr_total = 0;
                mgr.tmp_bufs = [];
                mgr.tmp_bufs_len = 0;
            }
        },
        processHeader: function(mgr, buffer) {
            var endstr = "\r\n\r\n";
            var oldLen = Buffer.byteLength(this.headerStr);
            this.headerStr += buffer.toString();
            var i = this.headerStr.indexOf(endstr);
            var offset = 0;

            if(i < 0) {
                return -1;
            } else {
                this.headerStr = this.headerStr.substring(0, i+endstr.length);
                offset = Buffer.byteLength(this.headerStr) - oldLen;
            }

            //Parse header now
            var arr = this.headerStr.split("\r\n");
            if(arr.length < 2) {
                console.log("parse header error");
                return -2;
            }

            var pattern = /\d{3,3}/;
            var match = pattern.exec(arr[0]);
            if(null == match) {
                console.log("parse header error, not matched");
                return -2;
            }
            //console.log(match);

            var header = {"status":0, "body":{}};
            var transfer_key = "transfer-encoding";
            var transfer_value = "chunked";
            var connecStr = "connection";
            header.status = parseInt(match[0]);
            for(i = 1; i < arr.length; i++) {
                var ss = arr[i].split(":");
                if(ss.length < 2) {
                    continue;
                }
                var sstrip = ss[0].replace(/^\s+|\s+$/g, "").toLowerCase();
                var ss1trip = ss[1].replace(/^\s+|\s+$/g, "").toLowerCase();
                if(sstrip == connecStr) {
                    continue;
                }
                if((sstrip == transfer_key)
                   && (ss1trip == transfer_value)) {
                    //this.isChunked = true;
                    //this.chunkSize = 0;
                    //this.chunkIsStart = false;
                }
                header.body[sstrip] = ss1trip;
            }

            /* if(typeof header.body.referer != "undefined") {
                var n = header.body.referer.lastIndexOf(":");
                header.body.referer = "http://127.0.0.1:8060" + header.body.referer.substring(n+3);
            } */
            this.headerOk = true;
            this.res.writeHead(header.status, header.body);
            this.headerStr = "";
            //console.log(header);

            if((buffer.length-offset) > 0) {
                var buf2 = buffer.slice(offset, buffer.length);
                this.innerProcessBuffer(mgr, buf2);
            }
            return 0;
        },
        toInsertJs: function(mgr, buffer) {
            var pattern = <title>([^<]*?)</title>;

            var oldLen = Buffer.byteLength(this.jsStr);
            this.jsStr += buffer.toString();

            var match = pattern.exec(this.jsStr);
            if((typeof match == 'undefined') || (null == match)) {
                return -1;
            }

            var start = match.index;
            var text = match[0];
            var end = start + text.length;
            this.jsStr = this.jsStr.substring(0, end);
            offset = Buffer.byteLength(this.jsStr) - oldLen;
            var buf2 = buffer.slice(offset, buffer.length);
            this.jsStr += '<script src="/__custom.js"></script>' + buf2.toString();
            this.res.write(this.jsStr);
            this.jsInsert = true;
            ths.jsStr = '';
        }
    };

    return HttpProcessor;
})();


//负责处理Forward Stream的数据
var forwardProcessor = (function () {
    function ForwardProcessor(seq) {
        this.seq = seq;
    }
    ForwardProcessor.prototype = {
        constructor:ForwardProcessor,
        processBuffer: function(mgr, buffer) {
        },
        test: function() {
        }
    };
    return ForwardProcessor;
})();

//负责管理用户的各项数据
var userMgmr = (function () {
    function UserMgmr(user) {
        this.user = user
        this.connections = {};
        this.curr_device = '';         //The current connection name
        this.seq = 0;
        this.index2obj = [];
        this.seq2index = [];
        this.cnt = 0;
        this.curr_seq = null;
        this.curr_total = 0;
    }

    UserMgmr.prototype = {
        constructor:UserMgmr,
        getConn:function() {
            return this.connections[this.curr_device];
        },
        addConn:function(conn, name) {
            this.connections[name] = conn;
        },
        setCurr:function(name) {
            this.curr_device = name;
        },
        getCurr:function() {
            return this.curr;
        },
        getSeqCnt:function() {
            return this.cnt;
        },
        getBySeq:function(seq) {
            if(seq >= this.seq2index.length) {
                return null;
            }
            var index = this.seq2index[seq];
            if(null == index) {
                return null;
            }

            return this.index2obj[index];
        },
        newSeq:function(obj_callback) {
            if(this.cnt >= userMgrMax) {
                return null;
            }

            var seq = this.seq;
            if(seq >= userMgrMax) {
                seq = 0;
            }
            if(this.index2obj.length <= seq) {
                this.index2obj.push(null);
                this.seq2index.push(null);
            }

            var obj = obj_callback(seq);
            this.seq2index[seq] = this.cnt;
            this.index2obj[this.cnt] = obj;
            this.cnt += 1;
            this.seq = seq+1;

            return obj;
        },
        delSeq:function(seq) {
            if(seq >= this.index2obj.length) {
                return null;
            }
            var index = this.seq2index[seq];
            if(null == index) {
                return null;
            }
            var obj = this.index2obj[index];
            this.cnt -= 1;
            if(index == this.cnt) {
                this.index2obj[index] = null;
            }
            else {
                this.index2obj[index] = this.index2obj[this.cnt];
                this.seq2index[this.index2obj[this.cnt].seq] = index;
                this.index2obj[this.cnt] = null;
            }
            return obj;
        }
    };

    return UserMgmr;
})();

var parseCookie = express.cookieParser();
var MemoryStore = express.session.MemoryStore;
var store = new MemoryStore();
var sessionHandler = express.session({
    secret: "session-secret",
    store: store,
});

app.configure(function() {
//    app.use(express.static(__dirname + "/public"));
//    app.set('views', __dirname);
//    app.set('view engine', 'ejs');
    app.use(parseCookie);
    //app.use(express.session({secret: "stringaaa"}));
    app.use(sessionHandler);
    app.use(function(req, res, next){
        req.pipe(concat(function(data){
        req.body = data;
        next();
        }));
    });
});

app.listen(cfg.port, '0.0.0.0');

var wsServer = new WebSocketServer({
    httpServer: app,

    // Firefox 7 alpha has a bug that drops the
    // connection on large fragmented messages
    fragmentOutgoingMessages: false
});

var router = new WebSocketRouter();
router.attachServer(wsServer);

function originIsAllowed(url) {
  // put logic here to detect whether the specified origin is allowed.
  return true;
}

/*
wsServer.on('request', function(request) {
    var url = request.httpRequest.url;
    if (!originIsAllowed(url)) {
      request.reject();
      console.log((new Date()) + ' Connection from origin ' + request.origin + ' rejected.');
      return;
    }

    var connection = request.accept('dumb-increment-protocol', request.origin);
    console.log((new Date()) + ' Connection accepted.');
    connections.push(connection);
    connection.on('message', function(message) {
        if (message.type === 'utf8') {
            console.log('Received Message: ' + message.utf8Data);
            connection.sendUTF(message.utf8Data);
        }
        else if (message.type === 'binary') {
            console.log('Received Binary Message of ' + message.binaryData.length + ' bytes');
            connection.sendBytes(message.binaryData);
        }
    });
    connection.on('close', function(reasonCode, description) {
        console.log((new Date()) + ' Peer ' + connection.remoteAddress + ' disconnected.');

        var index = connections.indexOf(connection);
        if (index !== -1) {
            connections.splice(index, 1);
        }
    });
});
*/

function createReq(bufs, type, seq) {
    var total_len = 16;

    var buffer = new Buffer(16);
    var bufs2 = [buffer];

    for(var i = 0; i < bufs.length; i++) {
        total_len += bufs[i].length;
        bufs2.push(bufs[i]);
    }

    buffer.writeUInt32BE(0x10293874, 0);   //Magic     0~4
    buffer.writeUInt8(0x01, 4);       //version   4~5
    buffer.writeUInt8(type, 5);             //request   5~6
    buffer.writeUInt16BE(seq, 6);          //seq        6~8
    buffer.writeUInt32BE(total_len, 8);    //length    8~12
    buffer.writeUInt32BE(0x00000000, 12);   //Reserve   12~16

    console.log("createReq the total len is " + total_len);

    return Buffer.concat(bufs2, total_len);
}

function createReqBuffers(bufs, type, seq) {
    var total_len = 16;

    var buffer = new Buffer(16);
    var bufs2 = [buffer];

    for(var i = 0; i < bufs.length; i++) {
        total_len += bufs[i].length;
        bufs2.push(bufs[i]);
    }

    buffer.writeUInt32BE(0x10293874, 0);   //Magic     0~4
    buffer.writeUInt8(0x01, 4);       //version   4~5
    buffer.writeUInt8(type, 5);             //request   5~6
    buffer.writeUInt16BE(seq, 6);          //seq        6~8
    buffer.writeUInt32BE(total_len, 8);    //length    8~12
    buffer.writeUInt32BE(0x00000000, 12);   //Reserve   12~16

    console.log("createReq the total len is " + total_len);

    return bufs2;
}

function parseHeaderMessage(buffer) {
    var header = {};
    header.magic = buffer.readUInt32BE(0);
    header.ver = buffer.readUInt8(4);
    header.type = buffer.readUInt8(5);
    header.seq = buffer.readUInt16BE(6);
    header.length = buffer.readUInt32BE(8);
    return header;
}

function httpNormalParse(mgr, buf) {
    var processor = mgr.getBySeq(mgr.curr_seq);
    //console.log("parse normal message");
    if(null == processor) {
        mgr.curr_seq = null;
        mgr.curr_total = 0;
        console.log("parse normal message, but the seq is error");
        return;
    }

    //processor is web for tcp forward
    processor.processBuffer(mgr, buf);
}

function parseTunnelReq(mgr, conn, buffer, protoHeader) {
    var tmpseq = protoHeader.seq;
    var auth = buffer.readUInt32BE(16);
    console.log("seq " + tmpseq + " " + auth);

    var filePath = path.join(__dirname, 'config.json');
    var readStream = fileSystem.createReadStream(filePath);
    var filestr = "";
    readStream.on('data', function(data) {
        filestr += data;
    });
    readStream.on('end', function() {
        console.log("config.json " + filestr);
        var tmp_len = Buffer.byteLength(filestr) + 4;
        var tmp_buf = new Buffer(tmp_len);
        tmp_buf.writeUInt32BE(tmp_len, 0);
        tmp_buf.write(filestr, 4);
        var buf2 = createReq([tmp_buf], 0x6, tmpseq);
        conn.sendBytes(buf2);
    });
}

function parseNormalMessage(conn, buffer) {
    var mgr = userMap.get(conn.user);

    if(null == mgr.curr_seq) {
        console.log("parse the first message");
        if(buffer.length < protocolHeaderLen) {
            console.log("the buffer is too smaller " + buffer.length);
            mgr.tmp_bufs.push(buffer);
            mgr.tmp_bufs_len += buffer.length;
            if(mgr.tmp_bufs_len >= protocolHeaderLen) {
                var new_buf = Buffer.concat(mgr.tmp_bufs);
                parseNormalMessage(conn, new_buf);
            }
            return;
        } else {
            mgr.tmp_bufs = [];
            mgr.tmp_bufs_len = 0;
        }

        // Wait for first message
        var protoHeader = parseHeaderMessage(buffer);
        if((protoHeader.magic != 0x10293874)
           || (protoHeader.ver != 0x1)) {
            console.log("get first message error " + protoHeader);
            return;
        }

        if(protoHeader.type == 0x5) {
            parseTunnelReq(mgr, conn, buffer, protoHeader);
            return;
        }

        mgr.curr_seq = protoHeader.seq;
        mgr.curr_total = protoHeader.length;
        console.log("get curr_seq = " + mgr.curr_seq + "total len = " + mgr.curr_total);
        mgr.curr_total -=  protocolHeaderLen; //exclude the header length

        if(buffer.length > protocolHeaderLen) {
            //var buf2 = new Buffer(buffer.length-protocolHeaderLen);
            //buffer.copy(buf2, 0, protocolHeaderLen, buffer.length);
            var buf2 = buffer.slice(protocolHeaderLen, buffer.length);
            console.log("the first message still have some buffers");
            httpNormalParse(mgr, buf2);
        }
    }
    else {
        httpNormalParse(mgr, buffer);
    }
}

function connectionParse(connection, message) {
    var buffer = message.binaryData;

    if(typeof connection.user == "undefined") {
        var protoHeader = parseHeaderMessage(buffer);
        if((protoHeader.magic != 0x10293874)
           || (protoHeader.ver != 0x1)
           || (protoHeader.type != 0x3)) {
               console.log("parse handshake message error, header is " + header);
            return;
        }

        var o = JSON.parse(buffer.toString("utf8", 16));
        connection.user = o.username;
        connection.host = o.host;
        connection.port = o.port;
        console.log("get username= " + o.username + " host " + o.host + " port " + o.port);

    /* if(o.username == 'janson') {
        connection.close();
        return;
    } */

        var buf = new Buffer(4);
        buf.writeUInt32BE(0x1234, 0);
        connection.sendBytes(createReq([buf], 0x3, 0x0));

        var mgr = userMap.get(o.username);
        if(typeof mgr == 'undefined' || null == mgr) {
            mgr = new userMgmr(o.username);
        }
        var devicename = Math.floor(Math.random() * 100000) + 1;
        connection.devicename = devicename + '';
        mgr.addConn(connection, connection);
        console.log("add user " + connection.user + " devicename " + connection.devicename + " to userMap");
        return;
    }

    parseNormalMessage(connection, message.binaryData);
}
function getSidFromCookies(cookies) {
     var filtered = cookies.filter(function(obj) {
         return obj.name == 'connect.sid';
     });
     return filtered.length > 0 ? filtered[0].value : null;
}

function textProcess(conn) {
    console.log(message.utf8Data);
    if((typeof conn.sess_username == "undefined")
       || (typeof conn.sess_devicename == 'undefined')) {
        // Check login
        connection.close();
        return false;
    }

    var mgr = userMap.get(conn.sess_username);
    if((typeof mgr == 'undefined') || (null == mgr)) {
        return false;
    }

    if(typeof conn.seq == 'undefined') {
        if(message.utf8Data != 'hello') {
            return false;
        }

        // From browser, Parse handshake
        mgr.newSeq(function (seq) {
            conn.seq = seq;
            var processor = new forwardProcessor(seq);
            return processor;
        });

        var obj = {};
        obj.seq = conn.seq;
        obj.type = "handshake";
        obj.message = "hello";
        conn.sendUTF(JSON.stringify(obj));
    }

    //Message from browser, forward to web-tunnel client
    try {
        var obj = JSON.parse(message.utf8Data);
        if(obj.seq != conn.seq) {
            console.log("Forward websocket, the seq is error");
            return false;
        }

        var client_conn = mgr.getCurr();
        if((typeof client_conn == "undefined") || (null == client_conn)) {
            console.log("client connection losted");
            return false;
        }

        if(obj.type == "openning") {
            //event to client, prepare for telnet
            var buf = new Buffer(4);
            var port = parseInt(obj.message);
            buf.writeUInt16BE(conn.seq, port);
            var bufs = createReq([buf], 0x10, conn.seq);
            client_conn.sendBytes(bufs);
        }
        else {
            //Just forward to client
            var buf = new Buffer(obj.message);
            var bufs = createReq([buf], 0x11, conn.seq);
            client_conn.sendBytes(bufs);
        }

        return true;
    } catch(e) {
        return false;
    }
}

function newConnection(request, sess) {
    var connection = request.accept(request.origin);

    if((typeof sess != "undefined")
          && (typeof sess.username != "undefined")) {
          connection.sess_username = sess.username;
          connection.sess_devicename = sess.devicename;
    }

    console.log((new Date()) + " dumb-increment-protocol connection accepted from " + connection.remoteAddress +
                " - Protocol Version " + connection.webSocketVersion + " origin - " + request.origin);

    //TODO wait for handshake, if timeout, delete it.
    connection.on('message', function(message) {
        if (message.type === 'utf8') {
            textProcess(connection);
        }
        else {
            connectionParse(this, message);
        }
    });
    connection.on('close', function(closeReason, description) {
        if(typeof this.user != "undefined") {
            userMap.remove(this.user);
        }
    });
}

router.mount('*', 'dumb-increment-protocol', function(request) {
    // Should do origin verification here. You have to pass the accepted
    // origin into the accept method of the request.
    parseCookie(request.httpRequest, null, function(err) {
        var connect_sid = getSidFromCookies(request.cookies);
        store.get(connect_sid, function(err, sess) {
            console.log("The session is " + sess + " connect.sid= " + connect_sid );
            newConnection(request, sess);
        });
    });

});

//Just test
app.get('/_testseq', function(req, res) {
    var a = new userMgmr();
    for(var i = 0; i < 10; i++) {
        console.log(a.newSeq("a"+i,"b"+i));
    }
    console.log("cnt is " + a.getSeqCnt() + "\n");
    for(var i = 0; i < 4; i++) {
        console.log(a.delSeq(i));
    }
    console.log("cnt is " + a.getSeqCnt() + "\n");
    for(var i = 0; i < 10; i++) {
        console.log(a.newSeq("a"+i,"b"+i));
    }
    console.log("cnt is " + a.getSeqCnt() + "\n");
    for(var i = 0; i < 4; i++) {
        console.log(a.index2obj[i]);
    }
    console.log("cnt is " + a.getSeqCnt() + "\n");
    res.send("oooo");
});

app.get("/__teststream", function(req, res) {
    var buf = new Buffer("ahahahaha");
    var readStream = Streamifier.createReadStream(buf);
    readStream.pipe(res);
});

app.get("/__testreq", function(req, res) {
    username = "janson";
    var mgr = userMap.get(username);
    if((typeof mgr == "undefined")
       || (null == mgr) ) {
        return;
    }

    var conn = mgr.getConn();
    var tmp_buf = new Buffer(4);
    tmp_buf.writeUInt32BE(0x88999988, 0);
    var bufs = createReq([tmp_buf], 0x5, 0x88);
    conn.sendBytes(bufs);
    res.send("again hello to you\n");
});

app.get("/__sessiontest", function(req, res) {
    var sess = req.session;
    res.send(sess.username + "\n");
});

function pipeFile(res, fileName) {
    var filePath = path.join(__dirname, 'public/' + fileName);
    var stat = fileSystem.statSync(filePath);

    res.writeHead(200, {
    'Content-Type': 'text/html; charset=UTF-8',
    'Content-Length': stat.size
    });

    var readStream = fileSystem.createReadStream(filePath);
    readStream.pipe(res);
}

//WebServer
//app.all
app.get("/__login", function(req, res) {
    pipeFile(res, 'login.html');
}

app.post("/__login", function(req, res) {
    var query = req.body.toString();
    var postObj = querystring.parse(query);
    var username = postObj.username;

    if(username == "") {
        res.send("Have to set the username !");
        return;
    }

    var mgr = userMap.get(username);
    if((typeof mgr == "undefined")
       || (null == mgr) ) {
            res.send("The device of username = " + username + " is not found !");
        return;
       }

    var sess = req.session;
    sess.username = username;
    res.redirect('/__devices');
});
app.get("/__logout", function(req, res) {
    username = "";
    if(typeof req.session.username != "undefined") {
    username = req.session.username;
        delete req.session.username;
    }
    res.send(username + " Logout!<br/><a href='/__login'>Login IN? </a>");
});

app.get("/__list", function(req, res){
    var sess = req.session;
    var obj = {};

    if(typeof sess.username == "undefined") {
        res.redirect("/__login");
        return;
    }

    var keys = [];
    userMap.forEach(function(value, key) {
        keys.push(key);
    });

    obj.keys = keys;

    res.writeHead(200, {
    "Content-Type" : "application/json"
    });
    res.end(JSON.stringify(obj));
});

app.get("/__iframe", function(req, res) {
    var sess = req.session;
    if(typeof sess.username == "undefined") {
        res.redirect('/__devices');
        return;
    }
    var username = sess.username;
    var mgr = userMap.get(username);
    if((typeof mgr == "undefined")
       || (null == mgr) ) {
        delete sess.username;
        res.redirect('/__devices');
        return;
    }

    pipFile('iframe.html');
});

app.get("/__devices", function(req, res) {
    pipFile('devices.html');
});

app.get("/__device/*", function(req, res) {
    var username = req.url.substring('/__device/'.length);

    if(username == "") {
        res.send("Not device name!");
        return;
    }

    var mgr = userMap.get(username);
    if((typeof mgr == "undefined")
       || (null == mgr) ) {
            res.send("The device of name = " + username + " is not found !");
        return;
       }

    var sess = req.session;
    sess.username = username;
    res.redirect('/__iframe');
});

app.all("/__(.*)", function(req, res) {
    var filePath = path.join(__dirname, 'public/' + req.params[0] + ".js");
    pipeFile(res, filePath);
});

app.all("*", function (req, res) {
    //console.log(req.session);
    var sess = req.session;
    if(typeof sess.username == "undefined") {
        res.redirect('/__devices');
        return;
    }
    var username = sess.username;
    var mgr = userMap.get(username);
    if((typeof mgr == "undefined")
       || (null == mgr) ) {
           delete sess.username;
        res.redirect('/__devices');
        return;
    }

    var conn = mgr.getConn();

    var ignores = ["accept-encoding", "connection"];

    var header = {}
    for(var p in req.headers) {
        if(ignores.indexOf(p.toLowerCase()) >= 0) {
            continue;
        }
        header[p.toLowerCase()] = req.headers[p];
    }
    if(conn.port == 80) {
        header.host = conn.host;
    } else {
        header.host = conn.host+":"+conn.port;
    }
    if(typeof header.referer != "undefined") {
    var tmp = "http://";
        var n = header.referer.indexOf(":", tmp.length);
    var n2 = header.referer.indexOf("/", n+1);
        header.referer = tmp + conn.host + header.referer.substring(n2);
    }

    var strHeader = req.method + " " + req.url + " HTTP/1.1\r\n";
    for(var p in header) {
        strHeader += p + ": " + header[p] + "\r\n";
    }
    strHeader += "connection: close\r\n\r\n";
    console.log("header is\n" + strHeader);

    var b1 = new Buffer(strHeader);
    var bufs = [b1];
    console.log(strHeader);
    if("POST" == req.method.toUpperCase()) {
        bufs.push(req.body);
        console.log(req.body.toString());
    }

   var processor = mgr.newSeq(function (seq) {
       return httpProcessor(req, res, seq);
   });
   console.log("new seq=" + processor.seq);

   var bufs2 = createReq2(bufs, 1, processor.seq);
   for(var i = 0; i < bufs2.length; i++) {
        conn.sendBytes(bufs2[i]);
   }
});
