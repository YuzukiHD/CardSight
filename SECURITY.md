# Security policy

## Supported versions

`master` and the latest published `0.1.x` source release are supported. Older commits and
downstream forks do not receive backports.

## Reporting a vulnerability

Do not open a public issue. Use GitHub's private vulnerability reporting form:

<https://github.com/YuzukiHD/CardSight/security/advisories/new>

Include the affected commit or release, the board and host setup, reproduction steps, and the
impact you believe the issue has. A proof of concept is welcome but not required.

## Scope

In scope:

- the firmware in `software/src/` and `software/apps/cardsight/`, including the USB mass-storage
  device, the storage-proxy write path, and the FAT/exFAT/ext4 parsers;
- the block layer and partition parsers;
- the WebAssembly simulator served by the documentation site, when the issue is in CardSight code.

Out of scope:

- vulnerabilities in third-party components that are unchanged by this project; report them to the
  upstream project and notify us if CardSight is affected;
- host operating systems, USB stacks, browsers and `xfel`;
- physical access, card cloning or attacks that require a modified board;
- the hardware design files, which are not published yet.

## Disclosure

We will acknowledge a report when it is received, investigate it, and coordinate a fix and a
disclosure date with the reporter. Please give us a reasonable window before publishing details.
