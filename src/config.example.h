#pragma once
#include <IPAddress.h>

// ================================================================
//  USER CONFIGURATION TEMPLATE
//  Instructions: 
//  1. Copy this file and rename it to "config.h"
//  2. Replace the placeholder values with your actual network credentials
//  3. Ensure "config.h" is included in .gitignore to prevent committing secrets
// ================================================================

// Wi-Fi Network Credentials
const char* WIFI_SSID    = "YOUR_WIFI_SSID";
const char* WIFI_PASS    = "YOUR_WIFI_PASSWORD";

// OTA Update Security
const char* OTA_PASSWORD = "YOUR_SECURE_OTA_PASSWORD";

// Device Hostname (Reachable locally at http://<HOSTNAME>.local)
const char* HOSTNAME     = "diningroom";

// Static IP Network Configuration
const IPAddress staticIP(192, 168, 1, 101);
const IPAddress gateway (192, 168, 1,   1);
const IPAddress subnet  (255, 255, 255, 0);
const IPAddress dns     (  8,   8,   8, 8);
