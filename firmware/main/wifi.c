#include "wifi.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "mdns.h"
#include "dhcpserver/dhcpserver.h"
#include "config.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_ap_netif  = NULL;
static bool     s_inited    = false;
static bool     s_connected = false;
static int      s_retries   = 0;
static volatile bool s_ap_join_pending = false;   // a client just joined the SoftAP
static bool     s_ever_connected = false;         // got an IP at least once this boot
static bool     s_keep_retrying  = false;         // provisioned-but-offline: retry forever
static char     s_mdns_host[32] = { 0 };          // actual advertised <host> (no .local)
#define STA_MAX_RETRY 5

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_ever_connected || s_keep_retrying) {
            // Joined once this boot, OR provisioned-but-offline (wifi_keep_retrying):
            // keep reconnecting forever so the device rides out router reboots /
            // outages over its long life. (The driver spaces these out.)
            esp_wifi_connect();
        } else if (s_retries < STA_MAX_RETRY) {
            s_retries++;
            ESP_LOGW(TAG, "STA disconnected; retry %d/%d", s_retries, STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);   // boot join failed -> open AP
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries        = 0;
        s_connected      = true;
        s_ever_connected = true;
        // Delegated mDNS names carry a literal address, so a new lease has to
        // be pushed into them or they keep advertising the old one - which is
        // worse than not answering, because a stale A record resolves.
        wifi_mdns_refresh_delegates();
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_join_pending = true;               // phone joined the setup AP
    }
}

// One-shot: true (once) after a client connects to the SoftAP, so the app can
// prompt "you're connected - visit the setup page". Clears on read.
bool wifi_ap_client_joined(void)
{
    bool j = s_ap_join_pending;
    s_ap_join_pending = false;
    return j;
}

void wifi_init(void)
{
    if (s_inited) return;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_events = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_inited = true;
}

bool wifi_connect_sta(const char *ssid, const char *pass, int timeout_ms)
{
    if (!s_inited) wifi_init();

    s_retries   = 0;
    s_connected = false;
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   // accept whatever the AP offers

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Joining '%s' (timeout %d ms)...", ssid, timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to '%s'", ssid);
        return true;
    }
    ESP_LOGW(TAG, "Could not join '%s'", ssid);
    return false;
}

// Make the SoftAP's DHCP server hand out its own address (192.168.4.1) as the
// DNS server, so clients send every lookup to our catch-all - without this the
// captive portal never triggers.
static void ap_offer_self_as_dns(void)
{
    esp_netif_dhcps_stop(s_ap_netif);

    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);

    dhcps_offer_t offer_dns = OFFER_DNS;           // enable the DNS DHCP option
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));

    esp_netif_dhcps_start(s_ap_netif);
}

void wifi_start_ap(const char *ap_ssid)
{
    if (!s_inited) wifi_init();

    esp_wifi_stop();                                // clean switch out of STA if it ran

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, ap_ssid, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len       = strlen(ap_ssid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = WIFI_AP_MAX_CONN;
    wc.ap.authmode       = WIFI_AUTH_OPEN;         // open network, no password

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ap_offer_self_as_dns();

    ESP_LOGI(TAG, "SoftAP '%s' up - join it, then browse to 192.168.4.1", ap_ssid);
}

bool wifi_is_connected(void)
{
    return s_connected;
}

// The boot join gave up (5 tries) but we ARE provisioned - keep trying forever in
// the background so the device auto-recovers when Wi-Fi returns, instead of being
// trapped in the setup AP over a transient outage. Resumes the attempts now.
void wifi_keep_retrying(void)
{
    s_keep_retrying = true;
    s_retries = 0;
    esp_wifi_connect();
}

const char *wifi_device_id(void)
{
    static char id[5] = { 0 };
    if (id[0]) return id;                          // computed once
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);        // from efuse; no wifi needed
    snprintf(id, sizeof(id), "%02X%02X", mac[4], mac[5]);
    return id;
}

// Hostname follows the device name so "<name>.local" is memorable. Sanitize to
// a valid mDNS label: lower-case, only [a-z0-9-], spaces/underscores -> '-', no
// doubled or trailing hyphens. If the name is empty or still the generic default
// ("magicmaker"), fall back to the per-device "magicmaker-<id>" so two unnamed
// units can't collide on the network.
//
// Separate from wifi_start_mdns() because the setup page needs to answer "where
// will this device be after it reboots?" - a question about a name that has been
// saved but not yet applied.
void wifi_hostname_for(const char *instance, char *out, size_t sz)
{
    if (!out || sz == 0) return;
    int n = 0;
    char prev = '-';
    for (const char *p = instance ? instance : ""; *p && n < (int)sz - 1; p++) {
        char c = (char)tolower((unsigned char)*p);
        if (c == '.') break;                         // a hostname is one label; stop
                                                     // at the first dot (e.g. drop ".local")
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n++] = c; prev = c;
        } else if ((c == ' ' || c == '-' || c == '_') && prev != '-' && n > 0) {
            out[n++] = '-'; prev = '-';
        }
    }
    while (n > 0 && out[n - 1] == '-') n--;         // strip trailing hyphen
    out[n] = '\0';

    if (n == 0 || strcmp(out, "magicmaker") == 0)   // unnamed / default -> unique
        snprintf(out, sz, "magicmaker-%s", wifi_device_id());
}

// ---------------------------------------------------------------------------
// The device answers on THREE names, because one name has to serve three jobs
// it can't all do well:
//
//   magicmaker.local        constant on every unit. THE SPOKEN NAME - short
//                           enough to say out loud and remember. "magicmaker
//                           dash zero eff eight five dot local" is not, and
//                           nobody transcribes it correctly while annoyed.
//   magicmaker-<id>.local   unique forever, derived from the MAC. THE PRINTED
//                           /QR NAME - nobody types it, which is exactly what
//                           made it a bad spoken name and makes it a good
//                           scanned one. Also the only name that reaches THIS
//                           reader once a house has two.
//   <their name>.local      the primary, whatever they called it. Daily use.
//
// The point isn't redundancy, it's that renaming used to silently break every
// written and spoken reference to the device. Now the first two are constants:
// a Help clip, the setup prompts, a printed card, anything written a year from
// now can name them and stay correct forever.
//
// The extras are DELEGATED hostnames, which carry an explicit address - so they
// have to be refreshed whenever the IP moves (DHCP renewal, reconnect after an
// outage). mdns_refresh_delegates() below does that, called from the GOT_IP
// handler.
static char s_delegate[2][32];      // the extra names actually registered
static int  s_delegate_n = 0;

static void delegate_set_addr(const char *name)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) return;

    mdns_ip_addr_t addr = { 0 };
    addr.addr.type = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4.addr = ip.ip.addr;
    addr.next = NULL;

    // add() first; if it's already there, set_address() moves it to the new IP.
    if (mdns_delegate_hostname_add(name, &addr) != ESP_OK)
        mdns_delegate_hostname_set_address(name, &addr);
}

void wifi_mdns_refresh_delegates(void)
{
    for (int i = 0; i < s_delegate_n; i++) delegate_set_addr(s_delegate[i]);
}

static void delegate_add(const char *name, const char *primary)
{
    // Skip the one that IS the primary - an unnamed device is already
    // "magicmaker-<id>", and delegating a name to itself is at best a no-op.
    if (strcmp(name, primary) == 0) return;
    if (s_delegate_n >= (int)(sizeof(s_delegate) / sizeof(s_delegate[0]))) return;

    strncpy(s_delegate[s_delegate_n], name, sizeof(s_delegate[0]) - 1);
    s_delegate[s_delegate_n][sizeof(s_delegate[0]) - 1] = '\0';
    s_delegate_n++;
    delegate_set_addr(name);
}

void wifi_start_mdns(const char *instance)
{
    static bool started = false;
    if (!started) {
        if (mdns_init() != ESP_OK) { ESP_LOGE(TAG, "mDNS init failed"); return; }
        started = true;
    }

    char host[32];
    wifi_hostname_for(instance, host, sizeof(host));

    strncpy(s_mdns_host, host, sizeof(s_mdns_host) - 1);   // remember for wifi_hostname()
    s_mdns_host[sizeof(s_mdns_host) - 1] = '\0';

    mdns_hostname_set(host);
    if (instance && instance[0]) mdns_instance_name_set(instance);

    // Re-registering from scratch on a rename: drop what we had first.
    for (int i = 0; i < s_delegate_n; i++) mdns_delegate_hostname_remove(s_delegate[i]);
    s_delegate_n = 0;

    char idhost[32];
    snprintf(idhost, sizeof(idhost), "magicmaker-%s", wifi_device_id());
    delegate_add("magicmaker", host);
    delegate_add(idhost,       host);

    ESP_LOGI(TAG, "mDNS also answering: %s", s_delegate_n ? s_delegate[0] : "(none)");
    if (s_delegate_n > 1) ESP_LOGI(TAG, "mDNS also answering: %s", s_delegate[1]);
    // Note: the browsable _http._tcp service is advertised later, when the
    // persistent LAN page ships with the countdown stage.
    ESP_LOGI(TAG, "mDNS up: %s.local  (\"%s\")", host, instance ? instance : "");
}

// The actual mDNS hostname in use (without the ".local"), e.g. "magic" so the
// device is at "magic.local". Follows the device name - NOT always
// "magicmaker-<id>". Empty until wifi_start_mdns() has run.
const char *wifi_hostname(void)
{
    return s_mdns_host;
}
