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
var userMgrMax = 256;
var protocolHeaderLen = 16;

var httpProcessor = (function () {
    function HttpProcessor(req, res, seq) {
        this.req = req;
        this.res = res;
        this.seq = seq;
        this.headerOk = false;
        this.headerStr = "";
        this.start = 0;
        this.isChunked = false;
        this.chunk_str = "";
    }
    HttpProcessor.prototype = {
        constructor:HttpProcessor,
        innerProcessBuffer: function(mgr, buffer) {
            //console.log(buffer.toString());
            //console.log("inner buffer len= " + buffer.length);
            this.res.write(buffer);
        },
        processBuffer: function(mgr, buffer) {
            //console.log("processBuffer");
            this.start += buffer.length;

            if(!this.headerOk) {
                this.processHeader(mgr, buffer);
            }
            else {
                //console.log("write response");
                this.innerProcessBuffer(mgr, buffer);
            }

            if(this.start >= mgr.curr_total) {
                this.res.end();
                mgr.delSeq(mgr.curr_seq);
                mgr.curr_seq = null;
                mgr.tmp_bufs = [];
                mgr.tmp_bufs_len = 0;
                mgr.curr_total = 0;
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
            //console.log(header);

            if((buffer.length-offset) > 0) {
                //var buf2 = new Buffer(buffer.length-offset);
                //buffer.copy(buf2, 0, offset, buffer.length);
                var buf2 = buffer.slice(offset, buffer.length);
                this.innerProcessBuffer(mgr, buf2);
            }
            return 0;
        }
    };

    return HttpProcessor;
})();

var userMgmr = (function () {

    function UserMgmr(conn) {
        this.conn = conn;
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
            return this.conn;
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
        newSeq:function(req, res) {
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

            var obj = new httpProcessor(req, res, seq);
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

app.configure(function() {
//    app.use(express.static(__dirname + "/public"));
//    app.set('views', __dirname);
//    app.set('view engine', 'ejs');
    app.use(express.cookieParser('some secret here'));
    app.use(express.session({secret: "stringaaa"}));
    app.use(function(req, res, next){
        req.pipe(concat(function(data){
        req.body = data;
        next();
        }));
    });
});

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

app.get("/_teststream", function(req, res) {
    var buf = new Buffer("ahahahaha");
    var readStream = Streamifier.createReadStream(buf);
    readStream.pipe(res);
});

//app.all("/__login
app.get("/__login", function(req, res) {
    var filePath = path.join(__dirname, 'public/__login.html');
    var stat = fileSystem.statSync(filePath);

    res.writeHead(200, {
    'Content-Type': 'text/html; charset=UTF-8',
    'Content-Length': stat.size
    });

    var readStream = fileSystem.createReadStream(filePath);
    // We replaced all the event handlers with a simple call to readStream.pipe()
    readStream.pipe(res);
});
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
    res.redirect('/');
});
app.get("/__logout", function(req, res) {
    username = "";
    if(typeof req.session.username != "undefined") {
    username = req.session.username;
        delete req.session.username;
    }
    res.send(username + " Logout!<br/><a href='/__login'>Login IN? </a>");
});

app.get("/__sessiontest", function(req, res) {
    var sess = req.session;
    res.send(sess.username + "\n");
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
    buffer.writeUInt16BE(0x0001, 4);       //version   4~6
    buffer.writeUInt16BE(type, 6);             //request   6~8
    buffer.writeUInt16BE(total_len, 8);    //length    8~10
    buffer.writeUInt16BE(seq, 10);          //seq        10~12
    buffer.writeUInt32BE(0x00000000, 12);   //Reserve   12~16

    console.log("createReq the total len is " + total_len);

    return Buffer.concat(bufs2, total_len);
}

function createReq2(bufs, type, seq) {
    var total_len = 16;

    var buffer = new Buffer(16);
    var bufs2 = [buffer];

    for(var i = 0; i < bufs.length; i++) {
        total_len += bufs[i].length;
        bufs2.push(bufs[i]);
    }

    buffer.writeUInt32BE(0x10293874, 0);   //Magic     0~4
    buffer.writeUInt16BE(0x0001, 4);       //version   4~6
    buffer.writeUInt16BE(type, 6);             //request   6~8
    buffer.writeUInt16BE(total_len, 8);    //length    8~10
    console.log("seq = " + seq);
    buffer.writeUInt16BE(seq, 10);          //seq        10~12
    buffer.writeUInt32BE(0x00000000, 12);   //Reserve   12~16

    console.log("createReq the total len is " + total_len);

    return bufs2;
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

    processor.processBuffer(mgr, buf);
}

function parseNormalMessage(conn, buffer) {
    var mgr = userMap.get(conn.user);

    //console.log("parseNormalMessage");
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
        var magic = buffer.readUInt32BE(0);
        var ver = buffer.readUInt16BE(4);
        var t = buffer.readUInt16BE(6)
        if((magic != 0x10293874)
           || (ver != 0x1)) {
               console.log("get first message error " + magic + " ver " + ver + " type " + t);
            return;
        }

	if(t == 0x5) {
		var tmpseq = buffer.readUInt16BE(10);
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

		return;
	}

        mgr.curr_total = buffer.readUInt16BE(8);
        mgr.curr_seq = buffer.readUInt16BE(10);
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
        var magic = buffer.readUInt32BE(0);
        var ver = buffer.readUInt16BE(4);
        var t = buffer.readUInt16BE(6)
        if((magic != 0x10293874)
           || (ver != 0x1)
           || (t != 0x3)) {
               console.log("parse handshake message error");
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

        var mgr = new userMgmr(connection);
        userMap.set(connection.user, mgr);
        console.log("add user " + connection.user + " to userMap")
        return;
    }

    parseNormalMessage(connection, message.binaryData);
}

router.mount('*', 'dumb-increment-protocol', function(request) {
    // Should do origin verification here. You have to pass the accepted
    // origin into the accept method of the request.
    var connection = request.accept(request.origin);
    console.log((new Date()) + " dumb-increment-protocol connection accepted from " + connection.remoteAddress +
                " - Protocol Version " + connection.webSocketVersion + " origin - " + request.origin);
    connection.on('message', function(message) {
        if (message.type === 'utf8') {
            console.log(message.utf8Data);
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
});

app.get("/__list", function(req, res){
    var sess = req.session;
    var obj = {};

    obj.device = "";
    if(typeof sess.username != "undefined") {
        obj.device = sess.username;
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

    var filePath = path.join(__dirname, 'public/__iframe.html');
    var stat = fileSystem.statSync(filePath);

    res.writeHead(200, {
    'Content-Type': 'text/html; charset=UTF-8',
    'Content-Length': stat.size
    });

    var readStream = fileSystem.createReadStream(filePath);
    readStream.pipe(res);
});

app.get("/__devices", function(req, res) {
    var filePath = path.join(__dirname, 'public/__devices.html');
    var stat = fileSystem.statSync(filePath);

    res.writeHead(200, {
    'Content-Type': 'text/html; charset=UTF-8',
    'Content-Length': stat.size
    });

    var readStream = fileSystem.createReadStream(filePath);
    readStream.pipe(res);
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

app.get("/__jquery-latest.min.js", function(req, res){
    var filePath = path.join(__dirname, 'public/jquery-latest.min.js');
    var stat = fileSystem.statSync(filePath);

    res.writeHead(200, {
    'Content-Type': 'text/html; charset=UTF-8',
    'Content-Length': stat.size
    });

    var readStream = fileSystem.createReadStream(filePath);
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

   var processor = mgr.newSeq(req, res);
   console.log("new seq=" + processor.seq);

   var bufs2 = createReq2(bufs, 1, processor.seq);
   for(var i = 0; i < bufs2.length; i++) {
    conn.sendBytes(bufs2[i]);
   }
});
