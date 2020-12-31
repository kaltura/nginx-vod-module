## Persistent proxy

OpenResty®-based proxy that supports persistence/stickiness, can be used to improve the hit ratio of nginx-vod-module shared memory caches.
The upstream servers are assumed to be k8s pods - the proxy periodically pulls the list of active servers from k8s via API.
The stickiness is maintained in redis and also in an nginx-lua shared dict.

### Features

* Persistence based on a key set in nginx.conf
* Fallback to another upstream server in case of error
* Load balancing based on 'power of two choices', using the number of errors and the % of fast responses for prioritization
* Exposes metrics in Prometheus format

### Setup

The proxy expects the following environment variables -
* `KUBERNETES_SERVICE_HOST` - k8s API endpoint host.
* `KUBERNETES_SERVICE_PORT` - k8s API endpoint port.
* `KUBERNETES_SERVICE_TOKEN` -  - k8s API token, usually read from `/var/run/secrets/kubernetes.io/serviceaccount/token`.
* `KUBERNETES_POD_NAMESPACE` - optional, the k8s namespace of the upstream pods, defaults to `vod`.
* `KUBERNETES_POD_APP_LABEL` - optional, the `app` label value of the upstream pods, if not specified, the upstream pods are not filtered by label.
* `REDIS_HOST` - the redis host, defaults to `127.0.0.1`.
* `REDIS_PORT` - optional, the redis port, defaults to `6379`.
