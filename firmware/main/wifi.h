// ---------------------------------------------------------------------------
// wifi.h - Wi-Fi bring-up: try to join the home network, or fall back to a
// SoftAP for provisioning. Stage 1 of the Wi-Fi/OTA roadmap.
//
// A network failure here must never stop the tap-show: every call returns and
// main() continues into the scanning loop regardless.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>

// One-time init: netif, default event loop, and the Wi-Fi driver (idle).
// Safe to call once at boot after NVS is initialised.
void wifi_init(void);

// Try to join `ssid`/`pass` as a station. Blocks up to `timeout_ms` waiting for
// an IP. Returns true if connected (got IP), false on timeout/auth failure.
bool wifi_connect_sta(const char *ssid, const char *pass, int timeout_ms);

// Start a SoftAP named `ap_ssid` (open network) for phone-based setup.
void wifi_start_ap(const char *ap_ssid);

// True while we currently hold an IP on the home network.
bool wifi_is_connected(void);

// After a failed boot join on a PROVISIONED device: keep retrying the connection
// forever in the background (auto-recover when Wi-Fi returns) instead of dropping
// to the setup AP. Call once when taking the stay-offline path.
void wifi_keep_retrying(void);

// One-shot: returns true once after a client joins the SoftAP (clears on read),
// so setup mode can prompt "you're connected - open the setup page".
bool wifi_ap_client_joined(void);

// Stable 4-hex-char per-device id from the MAC (e.g. "7A3F"). Uniquely names
// each board; used in the setup SSID and the mDNS hostname.
const char *wifi_device_id(void);

// Advertise this unit on the LAN as <host>.local, where <host> is derived from
// the device name (`instance`), falling back to magicmaker-<id> only for an
// empty/default name. `instance` is also the human-readable label. Call after
// joining.
void wifi_start_mdns(const char *instance);

// Re-point the extra mDNS names ("magicmaker", "magicmaker-<id>") at the
// current IP. They're delegated hostnames, which carry a literal address rather
// than tracking the interface, so a new DHCP lease has to be pushed into them -
// otherwise they keep answering with the old one, and a stale A record is worse
// than no answer because it resolves. Called from the GOT_IP handler.
void wifi_mdns_refresh_delegates(void);

// The advertised mDNS hostname in use (without ".local") - e.g. "magic" for a
// device named "magic". Empty string until wifi_start_mdns() has run.
const char *wifi_hostname(void);

// The hostname a given device name WOULD produce, without advertising it. The
// setup page needs this to tell the user where to find the reader after the
// reboot, at a point where the new name is saved but mDNS is still running under
// the old one (or, in AP mode, not running at all).
void wifi_hostname_for(const char *instance, char *out, size_t sz);
