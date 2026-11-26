# Onboard browser messaging fixture

The fixture serves the production page and its content security policy on an ephemeral loopback port. It uses the Meshtastic Python client's generated protobufs to read requests and emit replies. Run it in an environment with the Meshtastic Python package installed.

```sh
python3 mt_only/tests/stock_browser_page/fixture.py --port-file /tmp/zt-browser-port --result /tmp/zt-browser-observed.json
```

Open `http://127.0.0.1:<port>/` using the port written to the file. With Playwright CLI, run `run-code --filename=mt_only/tests/stock_browser_page/check.js` on this fixture session. A successful setup shows the Local and Trail channels and a single received message with literal markup from node `!12345678`. The fixture sends that message twice with the same ID. Send a broadcast and a directed Unicode message on Trail. The result file records the decoded destination, channel 3, distinct nonzero IDs and original text. The page reports routing acknowledgements. An invalid destination and a message longer than 233 UTF-8 bytes should leave the file unchanged. The fixture verifies the embedded script and stylesheet digests before it starts and binds only to 127.0.0.1.

Run `python3 mt_only/tests/stock_browser_page/check_tcp_close.py` for the TCP close regression. It uses the production FIN wait and real loopback sockets. Of 25 transactions, 24 clients release the connection after the declared content length. One client waits for the server's EOF. Linux also checks the number of server-side TIME_WAIT connections after each burst.

The XIAO browser profile reserves 12 Zephyr connection registrations and 12 contexts. The raw and HTTP listeners, Wi-Fi services, accepted connections and recent closes draw on those pools. The IPv4 defaults of four registrations and six contexts allowed the previous server's first active close to prevent the following SYN for the 1,500 ms TIME_WAIT interval. Waiting up to 500 ms for a client FIN after a framed response makes normal browser traffic close passively. When the raw companion owns the session, its HTTP conflict reply waits at most 15 ms. The transport keeps one request per connection and the standard TIME_WAIT interval.
