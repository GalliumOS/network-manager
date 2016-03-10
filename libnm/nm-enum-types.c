


/* Generated by glib-mkenums. Do not edit */

#include "nm-enum-types.h"

#include "NetworkManager.h" 
#include "nm-access-point.h" 
#include "nm-active-connection.h" 
#include "nm-client.h" 
#include "nm-device-adsl.h" 
#include "nm-device-bond.h" 
#include "nm-device-bridge.h" 
#include "nm-device-bt.h" 
#include "nm-device-ethernet.h" 
#include "nm-device-generic.h" 
#include "nm-device-infiniband.h" 
#include "nm-device-modem.h" 
#include "nm-device-olpc-mesh.h" 
#include "nm-device-team.h" 
#include "nm-device-vlan.h" 
#include "nm-device-wifi.h" 
#include "nm-device-wimax.h" 
#include "nm-device.h" 
#include "nm-dhcp-config.h" 
#include "nm-ip-config.h" 
#include "nm-object.h" 
#include "nm-remote-connection.h" 
#include "nm-types.h" 
#include "nm-vpn-connection.h" 
#include "nm-vpn-editor-plugin.h" 
#include "nm-wimax-nsp.h" 
#include "nm-secret-agent-old.h" 
#include "nm-vpn-plugin-old.h"

GType
nm_client_permission_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_CLIENT_PERMISSION_NONE, "NM_CLIENT_PERMISSION_NONE", "none" },
        { NM_CLIENT_PERMISSION_ENABLE_DISABLE_NETWORK, "NM_CLIENT_PERMISSION_ENABLE_DISABLE_NETWORK", "enable-disable-network" },
        { NM_CLIENT_PERMISSION_ENABLE_DISABLE_WIFI, "NM_CLIENT_PERMISSION_ENABLE_DISABLE_WIFI", "enable-disable-wifi" },
        { NM_CLIENT_PERMISSION_ENABLE_DISABLE_WWAN, "NM_CLIENT_PERMISSION_ENABLE_DISABLE_WWAN", "enable-disable-wwan" },
        { NM_CLIENT_PERMISSION_ENABLE_DISABLE_WIMAX, "NM_CLIENT_PERMISSION_ENABLE_DISABLE_WIMAX", "enable-disable-wimax" },
        { NM_CLIENT_PERMISSION_SLEEP_WAKE, "NM_CLIENT_PERMISSION_SLEEP_WAKE", "sleep-wake" },
        { NM_CLIENT_PERMISSION_NETWORK_CONTROL, "NM_CLIENT_PERMISSION_NETWORK_CONTROL", "network-control" },
        { NM_CLIENT_PERMISSION_WIFI_SHARE_PROTECTED, "NM_CLIENT_PERMISSION_WIFI_SHARE_PROTECTED", "wifi-share-protected" },
        { NM_CLIENT_PERMISSION_WIFI_SHARE_OPEN, "NM_CLIENT_PERMISSION_WIFI_SHARE_OPEN", "wifi-share-open" },
        { NM_CLIENT_PERMISSION_SETTINGS_MODIFY_SYSTEM, "NM_CLIENT_PERMISSION_SETTINGS_MODIFY_SYSTEM", "settings-modify-system" },
        { NM_CLIENT_PERMISSION_SETTINGS_MODIFY_OWN, "NM_CLIENT_PERMISSION_SETTINGS_MODIFY_OWN", "settings-modify-own" },
        { NM_CLIENT_PERMISSION_SETTINGS_MODIFY_HOSTNAME, "NM_CLIENT_PERMISSION_SETTINGS_MODIFY_HOSTNAME", "settings-modify-hostname" },
        { NM_CLIENT_PERMISSION_LAST, "NM_CLIENT_PERMISSION_LAST", "last" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMClientPermission"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_client_permission_result_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_CLIENT_PERMISSION_RESULT_UNKNOWN, "NM_CLIENT_PERMISSION_RESULT_UNKNOWN", "unknown" },
        { NM_CLIENT_PERMISSION_RESULT_YES, "NM_CLIENT_PERMISSION_RESULT_YES", "yes" },
        { NM_CLIENT_PERMISSION_RESULT_AUTH, "NM_CLIENT_PERMISSION_RESULT_AUTH", "auth" },
        { NM_CLIENT_PERMISSION_RESULT_NO, "NM_CLIENT_PERMISSION_RESULT_NO", "no" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMClientPermissionResult"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_client_error_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_CLIENT_ERROR_FAILED, "NM_CLIENT_ERROR_FAILED", "failed" },
        { NM_CLIENT_ERROR_MANAGER_NOT_RUNNING, "NM_CLIENT_ERROR_MANAGER_NOT_RUNNING", "manager-not-running" },
        { NM_CLIENT_ERROR_OBJECT_CREATION_FAILED, "NM_CLIENT_ERROR_OBJECT_CREATION_FAILED", "object-creation-failed" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMClientError"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_vpn_editor_plugin_capability_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { NM_VPN_EDITOR_PLUGIN_CAPABILITY_NONE, "NM_VPN_EDITOR_PLUGIN_CAPABILITY_NONE", "none" },
        { NM_VPN_EDITOR_PLUGIN_CAPABILITY_IMPORT, "NM_VPN_EDITOR_PLUGIN_CAPABILITY_IMPORT", "import" },
        { NM_VPN_EDITOR_PLUGIN_CAPABILITY_EXPORT, "NM_VPN_EDITOR_PLUGIN_CAPABILITY_EXPORT", "export" },
        { NM_VPN_EDITOR_PLUGIN_CAPABILITY_IPV6, "NM_VPN_EDITOR_PLUGIN_CAPABILITY_IPV6", "ipv6" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("NMVpnEditorPluginCapability"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_wimax_nsp_network_type_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_WIMAX_NSP_NETWORK_TYPE_UNKNOWN, "NM_WIMAX_NSP_NETWORK_TYPE_UNKNOWN", "unknown" },
        { NM_WIMAX_NSP_NETWORK_TYPE_HOME, "NM_WIMAX_NSP_NETWORK_TYPE_HOME", "home" },
        { NM_WIMAX_NSP_NETWORK_TYPE_PARTNER, "NM_WIMAX_NSP_NETWORK_TYPE_PARTNER", "partner" },
        { NM_WIMAX_NSP_NETWORK_TYPE_ROAMING_PARTNER, "NM_WIMAX_NSP_NETWORK_TYPE_ROAMING_PARTNER", "roaming-partner" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMWimaxNspNetworkType"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}


