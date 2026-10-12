# Security Policy

## Supported versions

Security fixes are released for the current OpenSprinklerShop firmware line
(2.4.0, latest build) for all platforms built from this repository:
OpenSprinkler Pro (ESP32-C5), OpenSprinkler 3.x (ESP8266) and OpenSprinkler Pi.
Older builds receive fixes through the regular online update; there are no
separate maintenance branches.

The upstream OpenSprinkler firmware (2.2.x, opensprinkler.com) is a separate
project and is not covered by this policy.

## Reporting a vulnerability

Please do not open a public GitHub issue for security problems.

- Open a ticket at the OpenSprinklerShop support portal:
  <https://opensprinklershop.de/support> and start the subject with
  `[Security]`. Tickets are visible only to our staff, and attachments are
  accepted.
- If you need an encrypted channel, say so in the ticket and we will provide a
  PGP key or arrange another secure exchange.

Include the firmware version and build (Settings > About, e.g. `2.4.0 (229)`),
the hardware (OpenSprinkler Pro, 3.x or Pi), the affected file or endpoint,
steps to reproduce and, if you have one, a proposed fix.

## What to expect

- Acknowledgement within 3 working days.
- An assessment and a planned fix date within 14 days for confirmed issues.
- Fixes ship as a firmware build with a CHANGELOG entry; the reporter is
  credited there unless they prefer not to be named.
- We do not run a bug bounty program.

## Scope notes

- The device API authenticates with the MD5 hash of the device password
  (`pw=` parameter), as in the upstream OpenSprinkler protocol. Reports about
  MD5 as such are known; reports about bypasses, missing checks or secret
  leakage through the API are welcome.
- `Access-Control-Allow-Origin: *` on the device API is intentional: the web
  UI is served from a different origin. Authentication does not rely on
  cookies.
- Testing against devices you own is fine. Please do not test against
  `opensprinklershop.de`, `ui.opensprinklershop.de`, the update server or the
  cloud relay without prior agreement.
