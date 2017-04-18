#!/bin/sh
set -o nounset                              # Treat unset variables as an error

NGINX_VERSION=1.12.0
NGINX_URI="http://nginx.org/download/nginx-$NGINX_VERSION.tar.gz"


if [ ! -x "`which wget 2>/dev/null`" ];then
        echo "Need to install wget."
        exit 2
fi
mkdir -p /tmp/builddir/nginx-$NGINX_VERSION
cp -r . /tmp/builddir/nginx-$NGINX_VERSION/nginx-vod-module
cd /tmp/builddir
wget $NGINX_URI -O kaltura-nginx-$NGINX_VERSION.tar.gz
tar zxf kaltura-nginx-$NGINX_VERSION.tar.gz
cd nginx-$NGINX_VERSION

FFMPEG_VERSION=3.2
LD_LIBRARY_PATH=/opt/kaltura/ffmpeg-$FFMPEG_VERSION/lib
LIBRARY_PATH=/opt/kaltura/ffmpeg-$FFMPEG_VERSION/lib
C_INCLUDE_PATH=/opt/kaltura/ffmpeg-$FFMPEG_VERSION/include
export LD_LIBRARY_PATH LIBRARY_PATH C_INCLUDE_PATH

./configure \
        --prefix=/etc/nginx \
        --sbin-path=/sbin/nginx \
        --conf-path=/etc/nginx/nginx.conf \
        --error-log-path=/var/log/log/nginx/error.log \
        --http-log-path=/var/log/log/nginx/access.log \
        --pid-path=/var/log/run/nginx.pid \
        --lock-path=/var/log/run/nginx.lock \
        --http-client-body-temp-path=/var/log/cache/nginx/client_temp \
        --http-proxy-temp-path=/var/log/cache/nginx/proxy_temp \
        --http-fastcgi-temp-path=/var/log/cache/nginx/fastcgi_temp \
        --http-uwsgi-temp-path=/var/log/cache/nginx/uwsgi_temp \
        --http-scgi-temp-path=/var/log/cache/nginx/scgi_temp \
        --with-http_ssl_module \
        --with-http_realip_module \
        --with-http_addition_module \
        --with-http_sub_module \
        --with-http_dav_module \
        --with-http_flv_module \
        --with-http_mp4_module \
        --with-http_gunzip_module \
        --with-http_gzip_static_module \
        --with-http_random_index_module \
        --with-http_secure_link_module \
        --with-http_stub_status_module \
        --with-http_auth_request_module \
        --with-mail \
        --with-mail_ssl_module \
        --with-file-aio \
        --with-ipv6 \
        --with-debug \
        --with-threads \
        --with-cc-opt="-O3" \
        $*
make
