// SPDX-License-Identifier: GPL-3.0-only
// Included by both the Zephyr transport and the POSIX loopback fixture.
static void waitBrowserPeerClose(int fd,int timeoutMs) {
    const int64_t deadline=k_uptime_get()+timeoutMs;
    for(;;){
        const auto remaining=deadline-k_uptime_get();
        if(remaining<=0)return;
        zsock_pollfd poll{};poll.fd=fd;poll.events=ZSOCK_POLLIN;
        // EOF, pending pipelined input, a hangup or a socket error ends this
        // one-request connection. Never parse or consume any additional input.
        if(zsock_poll(&poll,1,int(remaining))>=0 || errno!=EINTR)return;
    }
}
