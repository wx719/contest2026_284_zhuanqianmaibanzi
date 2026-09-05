/****************************************************************************
 * vendor/openvela/boards/contest2026_284_board/src/velapoka_esp_hosted.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <debug.h>
#include <errno.h>
#include <netinet/arp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <syslog.h>

#include <nuttx/kmalloc.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/netdev.h>
#ifdef CONFIG_NET_PKT
#  include <nuttx/net/pkt.h>
#endif
#include <nuttx/spinlock.h>
#include <nuttx/wireless/wireless.h>
#include <nuttx/wqueue.h>

#include "esp_event.h"
#include "esp_wifi.h"
#include "os_wrapper.h"
#include "rpc_wrap.h"
#include "transport_drv.h"

#include "velapoka_esp_hosted.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_WIFI_WORK          LPWORK
#define VELAPOKA_WIFI_BUFSIZE       (CONFIG_NET_ETH_PKTSIZE + \
                                     CONFIG_NET_LL_GUARDSIZE + \
                                     CONFIG_NET_GUARDSIZE)
#define VELAPOKA_WIFI_SCAN_BUFSIZE  4096
#define VELAPOKA_IW_EVENT_SIZE(f)   \
  (offsetof(struct iw_event, u) + sizeof(((union iwreq_data *)0)->f))

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct velapoka_wifi_s
{
  struct net_driver_s dev;
  struct iob_queue_s rxqueue;
  struct iob_queue_s txqueue;
  struct work_s rxwork;
  struct work_s txwork;
  spinlock_t lock;
  transport_channel_t *channel;
  transport_channel_tx_fn_t tx;
  wifi_config_t config;
  uint8_t flatbuf[VELAPOKA_WIFI_BUFSIZE];
  uint8_t *scanbuf;
  size_t scanlen;
  bool ifup;
  bool initialized;
  bool connected;
  bool scan_ready;
  bool rxwork_running;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct velapoka_wifi_s g_wifi;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int velapoka_wifi_errno(int ret)
{
  if (ret == ESP_OK)
    {
      return OK;
    }

  if (ret == ESP_ERR_NO_MEM)
    {
      return -ENOMEM;
    }

  if (ret == ESP_ERR_INVALID_ARG)
    {
      return -EINVAL;
    }

  if (ret == ESP_ERR_TIMEOUT)
    {
      return -ETIMEDOUT;
    }

  return -EIO;
}

static void velapoka_wifi_cache_tx(struct velapoka_wifi_s *priv)
{
  if (priv->dev.d_iob != NULL)
    {
      iob_tryadd_queue(priv->dev.d_iob, &priv->txqueue);
    }

  netdev_iob_clear(&priv->dev);
}

static void velapoka_wifi_transmit(struct velapoka_wifi_s *priv)
{
  struct iob_s *iob;
  uint16_t llhdrlen = NET_LL_HDRLEN(&priv->dev);
  unsigned int offset = CONFIG_NET_LL_GUARDSIZE - llhdrlen;
  int ret;

  while ((iob = iob_remove_queue(&priv->txqueue)) != NULL)
    {
      iob_copyout(priv->flatbuf + llhdrlen, iob, iob->io_pktlen, 0);
      memcpy(priv->flatbuf, iob->io_data + offset, llhdrlen);
      ret = priv->tx(priv, priv->flatbuf, iob->io_pktlen + llhdrlen);
      if (ret != ESP_OK)
        {
          nwarn("ESP-Hosted TX failed: %d\n", ret);
        }

      iob_free_chain(iob);
    }
}

static int velapoka_wifi_txpoll(struct net_driver_s *dev)
{
  struct velapoka_wifi_s *priv = dev->d_private;

  velapoka_wifi_cache_tx(priv);
  velapoka_wifi_transmit(priv);
  return OK;
}

static void velapoka_wifi_txwork(void *arg)
{
  struct velapoka_wifi_s *priv = arg;

  net_lock();
  if (priv->ifup)
    {
      while (devif_poll(&priv->dev, velapoka_wifi_txpoll));
    }

  velapoka_wifi_transmit(priv);
  net_unlock();
}

static int velapoka_wifi_txavail(struct net_driver_s *dev)
{
  struct velapoka_wifi_s *priv = dev->d_private;

  if (work_available(&priv->txwork))
    {
      work_queue(VELAPOKA_WIFI_WORK, &priv->txwork,
                 velapoka_wifi_txwork, priv, 0);
    }

  return OK;
}

static struct iob_s *velapoka_wifi_rxdequeue(struct velapoka_wifi_s *priv)
{
  struct iob_s *iob;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);
  iob = iob_remove_queue(&priv->rxqueue);
  spin_unlock_irqrestore(&priv->lock, flags);
  return iob;
}

static void velapoka_wifi_rxwork(void *arg)
{
  struct velapoka_wifi_s *priv = arg;
  struct net_driver_s *dev = &priv->dev;
  struct eth_hdr_s *eth;
  struct iob_s *iob;
  irqstate_t flags;

  for (; ; )
    {
      net_lock();
      while ((iob = velapoka_wifi_rxdequeue(priv)) != NULL)
        {
          dev->d_iob = iob;
          dev->d_len = iob->io_pktlen;
          iob_reserve(iob, CONFIG_NET_LL_GUARDSIZE);

#ifdef CONFIG_NET_PKT
          pkt_input(dev);
#endif

          eth = (struct eth_hdr_s *)
            &iob->io_data[CONFIG_NET_LL_GUARDSIZE - NET_LL_HDRLEN(dev)];

#ifdef CONFIG_NET_IPv4
          if (eth->type == HTONS(ETHTYPE_IP))
            {
              ipv4_input(dev);
            }
          else
#endif
#ifdef CONFIG_NET_IPv6
          if (eth->type == HTONS(ETHTYPE_IP6))
            {
              ipv6_input(dev);
            }
          else
#endif
#ifdef CONFIG_NET_ARP
          if (eth->type == HTONS(ETHTYPE_ARP))
            {
              arp_input(dev);
            }
          else
#endif
            {
              dev->d_len = 0;
            }

          if (dev->d_len > 0)
            {
              velapoka_wifi_cache_tx(priv);
            }

          netdev_iob_release(dev);
        }

      velapoka_wifi_transmit(priv);
      net_unlock();

      flags = spin_lock_irqsave(&priv->lock);
      if (iob_peek_queue(&priv->rxqueue) == NULL)
        {
          priv->rxwork_running = false;
          spin_unlock_irqrestore(&priv->lock, flags);
          break;
        }

      spin_unlock_irqrestore(&priv->lock, flags);
    }
}

static int velapoka_wifi_rx(void *handle, void *buffer,
                            void *buffer_to_free, size_t len)
{
  struct velapoka_wifi_s *priv = handle;
  struct iob_s *iob;
  irqstate_t flags;
  bool schedule = false;
  int ret;

  if (!priv->ifup || len > VELAPOKA_WIFI_BUFSIZE)
    {
      return ESP_FAIL;
    }

  iob = iob_tryalloc(false);
  if (iob == NULL)
    {
      return ESP_ERR_NO_MEM;
    }

  iob_reserve(iob, CONFIG_NET_LL_GUARDSIZE - NET_LL_HDRLEN(&priv->dev));
  ret = iob_trycopyin(iob, buffer, len, 0, false);
  if (ret != len)
    {
      iob_free_chain(iob);
      return ESP_ERR_NO_MEM;
    }

  flags = spin_lock_irqsave(&priv->lock);
  ret = iob_tryadd_queue(iob, &priv->rxqueue);
  if (ret >= 0 && !priv->rxwork_running)
    {
      priv->rxwork_running = true;
      schedule = true;
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  if (ret < 0)
    {
      iob_free_chain(iob);
      return ESP_ERR_NO_MEM;
    }

  g_h.funcs->_h_free(buffer_to_free);
  if (schedule && work_queue(VELAPOKA_WIFI_WORK, &priv->rxwork,
                             velapoka_wifi_rxwork, priv, 0) < 0)
    {
      flags = spin_lock_irqsave(&priv->lock);
      priv->rxwork_running = false;
      spin_unlock_irqrestore(&priv->lock, flags);
      nwarn("ESP-Hosted RX worker queue failed\n");
    }

  return ESP_OK;
}

static int velapoka_wifi_ifup(struct net_driver_s *dev)
{
  struct velapoka_wifi_s *priv = dev->d_private;

  IOB_QINIT(&priv->rxqueue);
  IOB_QINIT(&priv->txqueue);
  priv->ifup = true;
  netdev_carrier_off(dev);
  return OK;
}

static int velapoka_wifi_ifdown(struct net_driver_s *dev)
{
  struct velapoka_wifi_s *priv = dev->d_private;

  priv->ifup = false;
  priv->connected = false;
  netdev_carrier_off(dev);
  iob_free_queue(&priv->rxqueue);
  iob_free_queue(&priv->txqueue);
  rpc_wifi_disconnect();
  return OK;
}

static int velapoka_wifi_connect(struct velapoka_wifi_s *priv)
{
  int ret;

  ret = rpc_wifi_set_config(WIFI_IF_STA, &priv->config);
  if (ret == ESP_OK)
    {
      ret = rpc_wifi_connect();
    }

  return velapoka_wifi_errno(ret);
}

static int velapoka_wifi_scan_format(struct velapoka_wifi_s *priv)
{
  wifi_ap_record_t *records;
  struct iw_event *iwe;
  uint16_t count = 0;
  size_t remain;
  size_t ssidlen;
  size_t aligned;
  int ret;
  int i;

  ret = rpc_wifi_scan_get_ap_num(&count);
  if (ret != ESP_OK || count == 0)
    {
      priv->scanlen = 0;
      return velapoka_wifi_errno(ret);
    }

  records = kmm_calloc(count, sizeof(*records));
  if (records == NULL)
    {
      return -ENOMEM;
    }

  ret = rpc_wifi_scan_get_ap_records(&count, records);
  if (ret != ESP_OK)
    {
      kmm_free(records);
      return velapoka_wifi_errno(ret);
    }

  priv->scanlen = 0;
  for (i = 0; i < count; i++)
    {
      remain = VELAPOKA_WIFI_SCAN_BUFSIZE - priv->scanlen;
      ssidlen = strnlen((char *)records[i].ssid, sizeof(records[i].ssid));
      aligned = (ssidlen + 3) & ~3;
      if (remain < VELAPOKA_IW_EVENT_SIZE(ap_addr) +
                   VELAPOKA_IW_EVENT_SIZE(essid) + aligned +
                   VELAPOKA_IW_EVENT_SIZE(qual) +
                   VELAPOKA_IW_EVENT_SIZE(mode) +
                   VELAPOKA_IW_EVENT_SIZE(data) +
                   VELAPOKA_IW_EVENT_SIZE(freq))
        {
          break;
        }

      iwe = (struct iw_event *)(priv->scanbuf + priv->scanlen);
      iwe->len = VELAPOKA_IW_EVENT_SIZE(ap_addr);
      iwe->cmd = SIOCGIWAP;
      iwe->u.ap_addr.sa_family = ARPHRD_ETHER;
      memcpy(iwe->u.ap_addr.sa_data, records[i].bssid, 6);
      priv->scanlen += iwe->len;

      iwe = (struct iw_event *)(priv->scanbuf + priv->scanlen);
      iwe->len = VELAPOKA_IW_EVENT_SIZE(essid) + aligned;
      iwe->cmd = SIOCGIWESSID;
      iwe->u.essid.pointer = (void *)sizeof(iwe->u.essid);
      iwe->u.essid.length = ssidlen;
      iwe->u.essid.flags = 0;
      memcpy(&iwe->u.essid + 1, records[i].ssid, ssidlen);
      priv->scanlen += iwe->len;

      iwe = (struct iw_event *)(priv->scanbuf + priv->scanlen);
      iwe->len = VELAPOKA_IW_EVENT_SIZE(qual);
      iwe->cmd = IWEVQUAL;
      iwe->u.qual.level = records[i].rssi;
      iwe->u.qual.updated = IW_QUAL_DBM | IW_QUAL_ALL_UPDATED;
      priv->scanlen += iwe->len;

      iwe = (struct iw_event *)(priv->scanbuf + priv->scanlen);
      iwe->len = VELAPOKA_IW_EVENT_SIZE(mode);
      iwe->cmd = SIOCGIWMODE;
      iwe->u.mode = IW_MODE_MASTER;
      priv->scanlen += iwe->len;

      iwe = (struct iw_event *)(priv->scanbuf + priv->scanlen);
      iwe->len = VELAPOKA_IW_EVENT_SIZE(data);
      iwe->cmd = SIOCGIWENCODE;
      iwe->u.data.flags = records[i].authmode == WIFI_AUTH_OPEN ?
                          IW_ENCODE_DISABLED :
                          IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
      iwe->u.data.length = 0;
      iwe->u.data.pointer = NULL;
      priv->scanlen += iwe->len;

      iwe = (struct iw_event *)(priv->scanbuf + priv->scanlen);
      iwe->len = VELAPOKA_IW_EVENT_SIZE(freq);
      iwe->cmd = SIOCGIWFREQ;
      iwe->u.freq.m = records[i].primary;
      iwe->u.freq.e = 0;
      priv->scanlen += iwe->len;
    }

  kmm_free(records);
  return OK;
}

static int velapoka_wifi_ioctl(struct net_driver_s *dev, int cmd,
                               unsigned long arg)
{
  struct velapoka_wifi_s *priv = dev->d_private;
  struct iwreq *iwr = (struct iwreq *)arg;
  struct iw_encode_ext *ext;
  wifi_scan_config_t scan;
  wifi_ap_record_t ap;
  size_t len;
  int auth;
  int ret = OK;

  memset(&scan, 0, sizeof(scan));

  switch (cmd)
    {
      case SIOCSIWESSID:
        if (iwr->u.essid.length > sizeof(priv->config.sta.ssid))
          {
            return -EINVAL;
          }

        memset(priv->config.sta.ssid, 0, sizeof(priv->config.sta.ssid));
        memcpy(priv->config.sta.ssid, iwr->u.essid.pointer,
               iwr->u.essid.length);
        if (iwr->u.essid.flags == IW_ESSID_ON)
          {
            ret = velapoka_wifi_connect(priv);
          }
        else if (iwr->u.essid.flags == IW_ESSID_OFF)
          {
            ret = velapoka_wifi_errno(rpc_wifi_disconnect());
          }
        break;

      case SIOCGIWESSID:
        len = strnlen((char *)priv->config.sta.ssid,
                      sizeof(priv->config.sta.ssid));
        if (iwr->u.essid.length < len)
          {
            return -E2BIG;
          }

        memcpy(iwr->u.essid.pointer, priv->config.sta.ssid, len);
        iwr->u.essid.length = len;
        iwr->u.essid.flags = priv->connected ? IW_ESSID_ON : IW_ESSID_OFF;
        break;

      case SIOCSIWENCODEEXT:
        ext = iwr->u.encoding.pointer;
        if (ext == NULL || ext->key_len > sizeof(priv->config.sta.password))
          {
            return -EINVAL;
          }

        memset(priv->config.sta.password, 0,
               sizeof(priv->config.sta.password));
        if (ext->alg != IW_ENCODE_ALG_NONE)
          {
            memcpy(priv->config.sta.password, ext->key, ext->key_len);
            priv->config.sta.pmf_cfg.capable = true;
          }
        break;

      case SIOCGIWENCODEEXT:
        ext = iwr->u.encoding.pointer;
        len = strnlen((char *)priv->config.sta.password,
                      sizeof(priv->config.sta.password));
        if (ext == NULL || iwr->u.encoding.length < sizeof(*ext) + len)
          {
            return -E2BIG;
          }

        ext->key_len = len;
        ext->alg = len == 0 ? IW_ENCODE_ALG_NONE : IW_ENCODE_ALG_CCMP;
        memcpy(ext->key, priv->config.sta.password, len);
        break;

      case SIOCSIWAP:
        priv->config.sta.bssid_set = true;
        memcpy(priv->config.sta.bssid, iwr->u.ap_addr.sa_data, 6);
        break;

      case SIOCGIWAP:
        if (priv->connected && rpc_wifi_sta_get_ap_info(&ap) == ESP_OK)
          {
            memcpy(iwr->u.ap_addr.sa_data, ap.bssid, 6);
          }
        else
          {
            memcpy(iwr->u.ap_addr.sa_data, priv->config.sta.bssid, 6);
          }

        iwr->u.ap_addr.sa_family = ARPHRD_ETHER;
        break;

      case SIOCSIWAUTH:
        auth = iwr->u.param.flags & IW_AUTH_INDEX;
        if (auth != IW_AUTH_WPA_VERSION)
          {
            return -ENOSYS;
          }

        switch (iwr->u.param.value)
          {
            case IW_AUTH_WPA_VERSION_DISABLED:
              priv->config.sta.threshold.authmode = WIFI_AUTH_OPEN;
              break;
            case IW_AUTH_WPA_VERSION_WPA:
              priv->config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
              break;
            case IW_AUTH_WPA_VERSION_WPA2:
              priv->config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
              break;
            case IW_AUTH_WPA_VERSION_WPA3:
              priv->config.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
              break;
            default:
              return -EINVAL;
          }
        break;

      case SIOCGIWAUTH:
        auth = iwr->u.param.flags & IW_AUTH_INDEX;
        if (auth != IW_AUTH_WPA_VERSION)
          {
            return -ENOSYS;
          }

        switch (priv->config.sta.threshold.authmode)
          {
            case WIFI_AUTH_OPEN:
              iwr->u.param.value = IW_AUTH_WPA_VERSION_DISABLED;
              break;
            case WIFI_AUTH_WPA_PSK:
              iwr->u.param.value = IW_AUTH_WPA_VERSION_WPA;
              break;
            case WIFI_AUTH_WPA2_PSK:
              iwr->u.param.value = IW_AUTH_WPA_VERSION_WPA2;
              break;
            case WIFI_AUTH_WPA3_PSK:
              iwr->u.param.value = IW_AUTH_WPA_VERSION_WPA3;
              break;
            default:
              iwr->u.param.value = IW_AUTH_WPA_VERSION_WPA2 |
                                   IW_AUTH_WPA_VERSION_WPA3;
              break;
          }
        break;

      case SIOCSIWMODE:
        ret = iwr->u.mode == IW_MODE_INFRA ? OK : -EOPNOTSUPP;
        break;

      case SIOCGIWMODE:
        iwr->u.mode = IW_MODE_INFRA;
        break;

      case SIOCSIWSCAN:
        scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        ret = velapoka_wifi_errno(rpc_wifi_scan_start(&scan, true));
        if (ret == OK)
          {
            ret = velapoka_wifi_scan_format(priv);
            priv->scan_ready = ret == OK;
          }
        break;

      case SIOCGIWSCAN:
        if (!priv->scan_ready)
          {
            return -EAGAIN;
          }

        if (iwr->u.data.pointer == NULL ||
            iwr->u.data.length < priv->scanlen)
          {
            iwr->u.data.length = priv->scanlen;
            return -E2BIG;
          }

        memcpy(iwr->u.data.pointer, priv->scanbuf, priv->scanlen);
        iwr->u.data.length = priv->scanlen;
        priv->scan_ready = false;
        break;

      case SIOCGIWSENS:
        ret = velapoka_wifi_errno(rpc_wifi_sta_get_rssi(&auth));
        if (ret == OK)
          {
            iwr->u.sens.value = auth;
            iwr->u.sens.fixed = 1;
          }
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

static void velapoka_wifi_event(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
  struct velapoka_wifi_s *priv = arg;

  if (id == WIFI_EVENT_STA_CONNECTED)
    {
      priv->connected = true;
      netdev_carrier_on(&priv->dev);
    }
  else if (id == WIFI_EVENT_STA_DISCONNECTED)
    {
      priv->connected = false;
      netdev_carrier_off(&priv->dev);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapoka_wifi_initialize(void)
{
  struct velapoka_wifi_s *priv = &g_wifi;
  wifi_init_config_t init =
{
  .static_rx_buf_num = 10,
  .dynamic_rx_buf_num = 32,
  .tx_buf_type = 1,
  .dynamic_tx_buf_num = 32,
  .ampdu_rx_enable = 1,
  .ampdu_tx_enable = 1,
  .rx_ba_win = 6,
  .beacon_max_len = 752,
  .mgmt_sbuf_num = 32,
  .espnow_max_encrypt_num = 7,
  .magic = WIFI_INIT_CONFIG_MAGIC,
};

  uint8_t mac[6];
  bool native_error = false;
  int ret;

  if (priv->initialized)
    {
      return OK;
    }

  memset(priv, 0, sizeof(*priv));
  IOB_QINIT(&priv->rxqueue);
  IOB_QINIT(&priv->txqueue);
  priv->config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  priv->config.sta.threshold.authmode = WIFI_AUTH_OPEN;
  priv->config.sta.pmf_cfg.capable = true;
  priv->scanbuf = kmm_malloc(VELAPOKA_WIFI_SCAN_BUFSIZE);
  if (priv->scanbuf == NULL)
    {
      return -ENOMEM;
    }

  ret = transport_drv_init(NULL);
  if (ret != ESP_OK)
    {
      goto errout;
    }

  priv->channel = transport_drv_add_channel(priv, ESP_STA_IF, false,
                                             &priv->tx,
                                             velapoka_wifi_rx);
  if (priv->channel == NULL)
    {
      ret = ESP_FAIL;
      goto errtransport;
    }

  ret = rpc_init();
  if (ret != ESP_OK)
    {
      goto errtransport;
    }

  ret = rpc_register_event_callbacks();
  if (ret != ESP_OK)
    {
      goto errrpc;
    }

  ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   velapoka_wifi_event, priv);
  if (ret != ESP_OK)
    {
      goto errevents;
    }

  ret = transport_drv_reconfigure();
  if (ret != ESP_OK || rpc_wifi_init(&init) != ESP_OK ||
      rpc_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
      rpc_wifi_start() != ESP_OK ||
      rpc_wifi_get_mac(WIFI_MODE_STA, mac) != ESP_OK)
    {
      ret = ESP_FAIL;
      goto errhandler;
    }

  priv->dev.d_ifup = velapoka_wifi_ifup;
  priv->dev.d_ifdown = velapoka_wifi_ifdown;
  priv->dev.d_txavail = velapoka_wifi_txavail;
  priv->dev.d_ioctl = velapoka_wifi_ioctl;
  priv->dev.d_private = priv;
  memcpy(priv->dev.d_mac.ether.ether_addr_octet, mac, sizeof(mac));

  ret = netdev_register(&priv->dev, NET_LL_IEEE80211);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted wlan0 registration failed: %d\n",
             ret);
      native_error = true;
      goto errhandler;
    }

  priv->initialized = true;
  syslog(LOG_INFO,
         "ESP-Hosted wlan0 MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return OK;

errhandler:
  esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               velapoka_wifi_event);
errevents:
  rpc_unregister_event_callbacks();
errrpc:
  rpc_deinit();
errtransport:
  transport_drv_deinit();
errout:
  kmm_free(priv->scanbuf);
  priv->scanbuf = NULL;
  return native_error ? ret : velapoka_wifi_errno(ret);
}
