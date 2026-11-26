// SPDX-License-Identifier: GPL-3.0-only
// Security negotiation may wait on Zephyr TX buffers for up to 30 seconds.
// It must not hold the session mutex needed by the main loop/watchdog.
#include "../src/BleTransport.cpp"
#include <thread>
#include <future>
#include <iostream>
using namespace mt::ble;
using namespace std::chrono_literals;
int main() {
 // This scenario intentionally leaves negotiation incomplete after the
 // blocking request returns; retain its explicit security callback below.
 mockAutoSecurity=false;
 assert(init(0x12345678)==0);
 bt_conn connection;
 std::promise<void> entered,release,progress;auto releaseReady=release.get_future();auto progressReady=progress.get_future();
 mockDuringSecurity=[&]{entered.set_value();releaseReady.wait();};
 std::thread connector([&]{callbacks.connected(&connection,0);});
 entered.get_future().wait();
 std::thread mainLoop([&]{const auto current=session();Packet p{};const auto status=security();assert(current&&!receive(p)&&status.connected&&status.pending&&status.level==1);progress.set_value();});
 const bool progressed=progressReady.wait_for(1s)==std::future_status::ready;
 release.set_value();connector.join();mainLoop.join();mockDuringSecurity={};
 assert(progressed&&connected()&&mockSecurity==BT_SECURITY_L2);
 assert(!security().pending&&security().request==0&&security().level==1);
 callbacks.security_changed(&connection,BT_SECURITY_L2,BT_SECURITY_ERR_SUCCESS);assert(security().level==2&&!security().error);
 bt_conn stale;callbacks.security_changed(&stale,1,BT_SECURITY_ERR_UNSPECIFIED);assert(security().level==2&&!security().error);
 callbacks.disconnected(&connection,0);assert(!security().connected&&security().level==2);
 mockSecurityResult=-ENOBUFS;callbacks.connected(&connection,0);
 assert(security().connected&&!security().pending&&security().request==-ENOBUFS&&security().level==1);
 callbacks.security_changed(&connection,1,BT_SECURITY_ERR_UNSPECIFIED);assert(security().error==9);
 callbacks.disconnected(&connection,0);assert(!security().connected&&security().request==-ENOBUFS&&security().error==9);
 std::cout<<"PASS blocked security request cannot hold session mutex or stall main loop progress\n";
}
