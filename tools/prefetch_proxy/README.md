## Segment prefetcher proxy for nginx-vod-module

OpenResty®-based proxy for optimizing the delivery of VOD streams to remote regions by prefetching video segments.
When the proxy gets a request for segment N, it starts pulling segments N+1 and N+2 in the background.
The segments that are pulled from upstream are cached using nginx's proxy_cache directive.
The proxy uses two types of caches -
* long term - holds manifests, init segments and segments 1-3. Since this cache holds only a few segments per video,
    the assumption is that it can hold the objects for relatively long period.
* short term - holds segment 4 and beyond - the purpose of this cache is to save the prefetched segments,
    until they are pulled by the CDN/client.

### Setup

Replace the `{{ origin_domain }}` markers in nginx.conf with the domain of the origin.
