#!/bin/sh
set -o nounset                              # Treat unset variables as an error


#build nginx-vod-module and deps

KALTURA_NGINX_VOD_VERSION=master
KALTURA_NGINX_VOD_URI="https://github.com/kaltura/nginx-vod-module/archive/$KALTURA_NGINX_VOD_VERSION.zip"

KALTURA_NGINX_AKAMAI_TOKEN_VALIDATE_VERSION=master
KALTURA_NGINX_AKAMAI_TOKEN_VALIDATE_URI="https://github.com/kaltura/nginx-akamai-token-validate-module/archive/$KALTURA_NGINX_AKAMAI_TOKEN_VALIDATE_VERSION.zip"
#
KALTURA_NGINX_SECURE_TOKEN_VERSION=master
KALTURA_NGINX_SECURE_TOKEN_URI="https://github.com/kaltura/nginx-secure-token-module/archive/$KALTURA_NGINX_SECURE_TOKEN_VERSION.zip"
NGINX_VERSION=1.6.2
NGINX_URI="http://nginx.org/download/nginx-$NGINX_VERSION.tar.gz"


if [ ! -x "`which wget 2>/dev/null`" ];then
        echo "Need to install wget."
        exit 2
fi
mkdir -p /tmp/builddir/nginx-$NGINX_VERSION
cp -r . /tmp/builddir/nginx-$NGINX_VERSION/nginx-vod-module-$KALTURA_NGINX_VOD_VERSION
cd /tmp/builddir
wget $NGINX_URI -O kaltura-nginx-$NGINX_VERSION.tar.gz
tar zxvf kaltura-nginx-$NGINX_VERSION.tar.gz
cd nginx-$NGINX_VERSION
wget $KALTURA_NGINX_SECURE_TOKEN_URI -O nginx-secure-token-module-$KALTURA_NGINX_SECURE_TOKEN_VERSION.zip
unzip -oqq nginx-secure-token-module-$KALTURA_NGINX_SECURE_TOKEN_VERSION.zip
wget $KALTURA_NGINX_AKAMAI_TOKEN_VALIDATE_URI -O nginx-akamai-token-validate-module-$KALTURA_NGINX_AKAMAI_TOKEN_VALIDATE_VERSION.zip
unzip -oqq nginx-akamai-token-validate-module-$KALTURA_NGINX_AKAMAI_TOKEN_VALIDATE_VERSION.zip
#wget $KALTURA_NGINX_VOD_URI -O nginx-vod-module-$KALTURA_NGINX_VOD_VERSION.zip
#unzip nginx-vod-module-$KALTURA_NGINX_VOD_VERSION.zip

export LD_LIBRARY_PATH=/opt/kaltura/ffmpeg-2.1.3/lib;
export LIBRARY_PATH=/opt/kaltura/ffmpeg-2.1.3/lib
export C_INCLUDE_PATH=/opt/kaltura/ffmpeg-2.1.3/include;
set -x
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
        --add-module=./nginx-vod-module-$KALTURA_NGINX_VOD_VERSION \
        --add-module=./nginx-secure-token-module-$KALTURA_NGINX_SECURE_TOKEN_VERSION \
        --add-module=./nginx-akamai-token-validate-module-$KALTURA_NGINX_AKAMAI_TOKEN_VALIDATE_VERSION \
        $*
make
