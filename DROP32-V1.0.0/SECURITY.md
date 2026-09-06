# Security policy

## Scope

D.R.O.P.32 is designed for trusted local networks. It does not bridge Wi-Fi connectivity to the USB-connected destination computer, but its management interface uses HTTP and ships with documented default credentials.

Do not expose the web interface to the public Internet. Change the default credentials in `src/main.cpp` before using the device on an untrusted shared network.

## Reporting a vulnerability

Please use GitHub's private vulnerability-reporting feature or open a GitHub Security Advisory for the repository. Do not publish credentials, exploit details or sensitive network information in a public issue.

Include the firmware version, board model, reproduction steps and observed impact. Reports will be reviewed on a best-effort basis.
