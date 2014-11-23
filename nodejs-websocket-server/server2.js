var express = require('express');
//var session = require('express-session')
var request = require('request');
var concat = require('concat-stream');
var HashMap = require('hashmap').HashMap;
var buffertools = require('buffertools');
var stream = require("stream");
var Streamifier = require('streamifier');
var http = require("http");

var app = express.createServer();

app.configure(function() {
    app.use(express.static(__dirname + "/public"));
    app.set('views', __dirname);
    app.set('view engine', 'ejs');
    app.use(express.cookieParser('some secret here'));
    app.use(express.session({secret: "stringaaa"}));
    app.use(express.bodyParser());
/*    app.use(function(req, res, next){
        req.pipe(concat(function(data){
        req.body = data;
        next();
        }));
    }); */
});

app.post("/posttest", function(req, res) {
    sess=req.session;
    sess.username=req.body.username
    res.send(req.body);
});

app.get("/chunktest", function(req, res) {
    sess=req.session;
    console.log("chunk test begin " + sess.username);
    res.setHeader('Content-Type', 'text/html; charset=UTF-8');
    res.setHeader("Transfer-Encoding", "chunked");
    //res.writeHead(200,{"Transfer-Encoding":"chunked"});
    res.write('<html><head>');
    res.write('<body>hahahah test hear');
    res.end('</body></html>');
});

app.get('/', function(req, res) {
    res.render('index', { layout: false });
});

app.get('/myip', function(req, res) {
    var ip = req.headers['x-forwarded-for'] || req.connection.remoteAddress;
    res.send(ip);
});

app.listen(7000, '0.0.0.0');
