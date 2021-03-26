Building:
```bash
meson build
ninja -C build
```

Help:
```
./fcgi_shortener
Usage: fcgi_shortener DB_PATH THREADS MIN_LENGTH
```

Running as an fcgi app:
```bash
spawn-fcgi -n -p 9000 -- fcgi_shortener /path/to/db 4 1
```

Pointing nginx at it:
```nginx
location / {
    fastcgi_param  GATEWAY_INTERFACE  CGI/1.1;
    fastcgi_param  SERVER_SOFTWARE    nginx;
    fastcgi_param  REQUEST_METHOD     $request_method;
    fastcgi_param  CONTENT_TYPE       $content_type;
    fastcgi_param  REQUEST_SCHEME     $scheme;
    fastcgi_param  HOST               $host;
    fastcgi_param  DOCUMENT_URI       $document_uri;

    fastcgi_pass  127.0.0.1:9000;
}
```

